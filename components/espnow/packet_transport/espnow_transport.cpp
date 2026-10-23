#include "espnow_transport.h"

#ifdef USE_ESP8266

#include "esphome/core/log.h"

namespace esphome::espnow {

static const char *const TAG = "espnow.transport";

bool ESPNowTransport::should_send() { return this->parent_ != nullptr && !this->parent_->is_failed(); }

void ESPNowTransport::setup() {
  PacketTransport::setup();

  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "ESPNow component not set");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "Registering ESP-NOW handlers, peer: %02X:%02X:%02X:%02X:%02X:%02X", this->peer_address_[0],
           this->peer_address_[1], this->peer_address_[2], this->peer_address_[3], this->peer_address_[4],
           this->peer_address_[5]);

  // Register received handler
  this->parent_->register_receive_handler(this);

  // Register broadcast handler
  this->parent_->register_broadcast_handler(this);
}

void ESPNowTransport::loop() {
  PacketTransport::loop();
  this->send_next_();
}

void ESPNowTransport::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP-NOW Packet Transport:");
  ESP_LOGCONFIG(TAG, "  Peer: %02X:%02X:%02X:%02X:%02X:%02X", this->peer_address_[0], this->peer_address_[1],
                this->peer_address_[2], this->peer_address_[3], this->peer_address_[4], this->peer_address_[5]);
  ESP_LOGCONFIG(TAG, "  TX queue capacity: %u", static_cast<unsigned>(TX_QUEUE_LIMIT));
  LOG_UPDATE_INTERVAL(this);
}

void ESPNowTransport::send_packet(const std::vector<uint8_t> &buf) const {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "ESPNow component not set");
    return;
  }

  if (buf.empty()) {
    ESP_LOGW(TAG, "Attempted to send empty packet");
    return;
  }

  if (buf.size() > ESPNOW_MAX_DATA_LEN) {
    ESP_LOGE(TAG, "Packet too large: %zu bytes (max %u)", buf.size(), (unsigned) ESPNOW_MAX_DATA_LEN);
    return;
  }

  if (this->tx_queue_.size() >= TX_QUEUE_LIMIT) {
    ESP_LOGW(TAG, "Transport TX queue full (%u packets), dropping len=%u", (unsigned) TX_QUEUE_LIMIT,
             (unsigned) buf.size());
    return;
  }

  this->tx_queue_.push_back(buf);
  this->send_next_();
}

void ESPNowTransport::send_next_() const {
  if (this->parent_ == nullptr || this->tx_in_flight_ || this->tx_queue_.empty())
    return;
  if (this->parent_->is_failed() || this->parent_->is_send_pending())
    return;

  this->tx_in_flight_ = true;
  const auto &packet = this->tx_queue_.front();
  int err = this->parent_->send(this->peer_address_.data(), packet.data(), packet.size(), [this](int status) {
    if (status != 0)
      ESP_LOGW(TAG, "Send failed: %d", status);
    if (!this->tx_queue_.empty())
      this->tx_queue_.pop_front();
    this->tx_in_flight_ = false;
    this->send_next_();
  });

  if (err != 0) {
    ESP_LOGW(TAG, "Unable to start send: %d; dropping packet", err);
    this->tx_in_flight_ = false;
    if (!this->tx_queue_.empty())
      this->tx_queue_.pop_front();
  }
}

bool ESPNowTransport::on_receive(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) {
  ESP_LOGV(TAG, "Received packet of size %u from %02X:%02X:%02X:%02X:%02X:%02X", (unsigned) size, info.src_addr[0],
           info.src_addr[1], info.src_addr[2], info.src_addr[3], info.src_addr[4], info.src_addr[5]);

  if (data == nullptr || size == 0) {
    ESP_LOGW(TAG, "Received empty or null packet");
    return false;
  }

  this->packet_buffer_.resize(size);
  memcpy(this->packet_buffer_.data(), data, size);
  this->process_(this->packet_buffer_);
  return false;  // Allow other handlers to run
}

bool ESPNowTransport::on_broadcast(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) {
  ESP_LOGV(TAG, "Received broadcast packet of size %u from %02X:%02X:%02X:%02X:%02X:%02X", (unsigned) size,
           info.src_addr[0], info.src_addr[1], info.src_addr[2], info.src_addr[3], info.src_addr[4], info.src_addr[5]);

  if (data == nullptr || size == 0) {
    ESP_LOGW(TAG, "Received empty or null broadcast packet");
    return false;
  }

  this->packet_buffer_.resize(size);
  memcpy(this->packet_buffer_.data(), data, size);
  this->process_(this->packet_buffer_);
  return false;  // Allow other handlers to run
}

}  // namespace esphome::espnow

#endif  // USE_ESP8266
