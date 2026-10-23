#include "espnow_component.h"

#ifdef USE_ESP8266

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

extern "C" {
#include <user_interface.h>
}

namespace esphome {
namespace espnow {

static const char *const TAG = "espnow";
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const char *espnow_role_to_string(uint8_t role) {
  switch (role) {
    case ESP_NOW_ROLE_CONTROLLER:
      return "CONTROLLER";
    case ESP_NOW_ROLE_SLAVE:
      return "SLAVE";
    case ESP_NOW_ROLE_COMBO:
      return "COMBO";
    default:
      return "UNKNOWN";
  }
}

static const char *wifi_opmode_to_string(uint8_t opmode) {
  switch (opmode) {
    case STATION_MODE:
      return "STA";
    case SOFTAP_MODE:
      return "SoftAP";
    case STATIONAP_MODE:
      return "STA+SoftAP";
    default:
      return "disabled/unknown";
  }
}

// ── Static member definitions ─────────────────────────────────────────────────

ESPNowComponent *global_esp_now = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

ESPNowQueueEntry ESPNowComponent::recv_queue_[ESPNOW_QUEUE_SIZE]{};
volatile uint8_t ESPNowComponent::recv_queue_write_ = 0;
volatile bool ESPNowComponent::send_cb_fired_ = false;
volatile uint8_t ESPNowComponent::send_cb_status_ = 0;

bool ESPNowComponent::should_manage_radio_() const {
#ifdef USE_WIFI
  // Do not interfere while the Wi-Fi component is enabled. It may briefly
  // turn the adapter off during its own restart/reconnect sequence.
  return wifi::global_wifi_component == nullptr || wifi::global_wifi_component->is_disabled();
#else
  return true;
#endif
}

bool ESPNowComponent::ensure_radio_ready_() {
  uint8_t opmode = wifi_get_opmode();
  if ((opmode & STATION_MODE) == 0) {
    ESP_LOGI(TAG, "Wi-Fi networking is disabled; enabling STA radio for ESP-NOW");
    // Arduino Core 3.x can leave the modem in forced sleep when Wi-Fi is off.
    // Wake it before changing opmode, matching ESP8266WiFiGenericClass::mode().
    if (wifi_fpm_get_sleep_type() != NONE_SLEEP_T) {
      wifi_fpm_do_wakeup();
      wifi_fpm_close();
    }
    ETS_UART_INTR_DISABLE();
    // Preserve an already-running fallback SoftAP while adding the Station
    // interface required by the CONTROLLER role.
    bool mode_ok = wifi_set_opmode_current(opmode | STATION_MODE);
    ETS_UART_INTR_ENABLE();
    if (!mode_ok) {
      ESP_LOGE(TAG, "Failed to enable STA radio for ESP-NOW");
      return false;
    }

    // Match ESPHome's ESP8266 Wi-Fi setup settling interval before using the
    // radio; the Non-OS SDK applies the mode transition asynchronously.
    delay(10);
    opmode = wifi_get_opmode();
    if ((opmode & STATION_MODE) == 0) {
      ESP_LOGE(TAG, "STA radio did not become ready (opmode=%u)", opmode);
      return false;
    }
  }

  // With Wi-Fi networking enabled, the associated AP owns the radio channel.
  // With Wi-Fi disabled (or absent), ESP-NOW must set the physical channel;
  // the channel stored in the peer entry does not tune the radio by itself.
  if (this->should_manage_radio_()) {
    if (this->channel_ == 0) {
      ESP_LOGW(TAG, "Wi-Fi networking is disabled and ESP-NOW channel is 0; using current radio channel %u",
               wifi_get_channel());
    } else if (wifi_get_channel() != this->channel_) {
      if (!wifi_set_channel(this->channel_)) {
        ESP_LOGE(TAG, "Failed to set ESP-NOW radio channel to %u", this->channel_);
        return false;
      }
      ESP_LOGD(TAG, "ESP-NOW radio channel set to %u", this->channel_);
    }
  }

  return true;
}

uint8_t ESPNowComponent::effective_peer_channel_(uint8_t configured_channel) const {
#ifdef USE_WIFI
  // Once ESPHome is associated, the AP owns the radio channel. A configured
  // ESP-NOW channel cannot override it, so peers must follow the live channel.
  if (wifi::global_wifi_component != nullptr && !wifi::global_wifi_component->is_disabled() &&
      wifi::global_wifi_component->is_connected()) {
    const uint8_t current_channel = wifi_get_channel();
    if (current_channel >= 1 && current_channel <= 14)
      return current_channel;
  }
#endif
  if (configured_channel != 0)
    return configured_channel;
  if (this->channel_ != 0)
    return this->channel_;
  return wifi_get_channel();
}

bool ESPNowComponent::synchronize_runtime_config_() {
  bool success = true;

  if (esp_now_get_self_role() != this->self_role_) {
    const int err = esp_now_set_self_role(static_cast<esp_now_role>(this->self_role_));
    if (err != 0) {
      ESP_LOGW(TAG, "Failed to restore ESP-NOW %s role: %d", espnow_role_to_string(this->self_role_), err);
      success = false;
    } else {
      ESP_LOGI(TAG, "Restored ESP-NOW %s role", espnow_role_to_string(this->self_role_));
    }
  }

  const uint8_t broadcast_channel = this->effective_peer_channel_(0);
  if (!esp_now_is_peer_exist(const_cast<uint8_t *>(BROADCAST_ADDR))) {
    const int err = esp_now_add_peer(const_cast<uint8_t *>(BROADCAST_ADDR), ESP_NOW_ROLE_SLAVE,
                                     broadcast_channel, nullptr, 0);
    if (err != 0) {
      ESP_LOGW(TAG, "Failed to restore broadcast peer on channel %u: %d", broadcast_channel, err);
      success = false;
    }
  } else {
    if (esp_now_get_peer_role(const_cast<uint8_t *>(BROADCAST_ADDR)) != ESP_NOW_ROLE_SLAVE &&
        esp_now_set_peer_role(const_cast<uint8_t *>(BROADCAST_ADDR), ESP_NOW_ROLE_SLAVE) != 0)
      success = false;
    if (esp_now_get_peer_channel(const_cast<uint8_t *>(BROADCAST_ADDR)) != broadcast_channel &&
        esp_now_set_peer_channel(const_cast<uint8_t *>(BROADCAST_ADDR), broadcast_channel) != 0)
      success = false;
  }

  for (uint8_t i = 0; i < this->static_peer_count_; i++) {
    ESPNowPeerConfig &cfg = this->static_peers_[i];
    const uint8_t peer_channel = this->effective_peer_channel_(cfg.channel);
    if (!esp_now_is_peer_exist(cfg.mac)) {
      uint8_t *key = cfg.encrypt ? cfg.lmk : nullptr;
      const uint8_t key_len = cfg.encrypt ? 16 : 0;
      const int err = esp_now_add_peer(cfg.mac, static_cast<esp_now_role>(cfg.role), peer_channel, key, key_len);
      if (err != 0) {
        ESP_LOGW(TAG, "Failed to restore peer %02X:%02X:%02X:%02X:%02X:%02X on channel %u: %d",
                 cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5], peer_channel, err);
        success = false;
      }
      continue;
    }

    if (esp_now_get_peer_role(cfg.mac) != cfg.role &&
        esp_now_set_peer_role(cfg.mac, static_cast<esp_now_role>(cfg.role)) != 0) {
      ESP_LOGW(TAG, "Failed to restore peer role for %02X:%02X:%02X:%02X:%02X:%02X", cfg.mac[0], cfg.mac[1],
               cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5]);
      success = false;
    }
    if (esp_now_get_peer_channel(cfg.mac) != peer_channel) {
      const int err = esp_now_set_peer_channel(cfg.mac, peer_channel);
      if (err != 0) {
        ESP_LOGW(TAG, "Failed to move peer %02X:%02X:%02X:%02X:%02X:%02X to channel %u: %d", cfg.mac[0],
                 cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5], peer_channel, err);
        success = false;
      } else {
        ESP_LOGI(TAG, "Peer %02X:%02X:%02X:%02X:%02X:%02X now follows radio channel %u", cfg.mac[0],
                 cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5], peer_channel);
      }
    }
  }

  return success;
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void ESPNowComponent::setup() {
  global_esp_now = this;

  // esp_now_init() can succeed while the ESP8266 radio is in NULL_MODE, but
  // no frames can be sent or received. ESPHome leaves the radio off when
  // wifi.enable_on_boot is false, so restore a radio-only STA interface first.
  if (!this->ensure_radio_ready_()) {
    this->mark_failed();
    return;
  }

  int err = esp_now_init();
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_init failed: %d", err);
    this->mark_failed();
    return;
  }

  // COMBO permits ESP8266 peers to both transmit and receive while retaining
  // the Station interface used by this component's radio setup.
  err = esp_now_set_self_role(static_cast<esp_now_role>(this->self_role_));
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_set_self_role(%s) failed: %d", espnow_role_to_string(this->self_role_), err);
    this->mark_failed();
    return;
  }

  err = esp_now_register_recv_cb(recv_cb_);
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %d", err);
    this->mark_failed();
    return;
  }

  err = esp_now_register_send_cb(send_cb_);
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_register_send_cb failed: %d", err);
    this->mark_failed();
    return;
  }

  if (has_pmk_) {
    // On ESP8266, the network key is called KoK (Key of Keys).
    err = esp_now_set_kok(pmk_, 16);
    if (err != 0) {
      ESP_LOGE(TAG, "esp_now_set_kok failed: %d", err);
      this->mark_failed();
      return;
    }
  }

  // Register the broadcast address so espnow.broadcast works without
  // requiring the user to list FF:FF:FF:FF:FF:FF in their peer config.
  if (!esp_now_is_peer_exist(const_cast<uint8_t *>(BROADCAST_ADDR))) {
    const uint8_t peer_channel = this->effective_peer_channel_(0);
    err = esp_now_add_peer(const_cast<uint8_t *>(BROADCAST_ADDR), ESP_NOW_ROLE_SLAVE, peer_channel, nullptr, 0);
    if (err != 0) {
      ESP_LOGE(TAG, "Failed to add broadcast peer: %d", err);
      this->mark_failed();
      return;
    }
  }

  for (uint8_t i = 0; i < static_peer_count_; i++) {
    const ESPNowPeerConfig &cfg = static_peers_[i];
    const uint8_t peer_channel = this->effective_peer_channel_(cfg.channel);
    uint8_t *key = cfg.encrypt ? const_cast<uint8_t *>(cfg.lmk) : nullptr;
    uint8_t key_len = cfg.encrypt ? 16 : 0;
    err = esp_now_add_peer(const_cast<uint8_t *>(cfg.mac), static_cast<esp_now_role>(cfg.role), peer_channel, key,
                           key_len);
    if (err != 0) {
      ESP_LOGW(TAG, "Failed to add static peer %02X:%02X:%02X:%02X:%02X:%02X on channel %u: %d",
               cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5], peer_channel, err);
    }
  }

  // Load persisted rolling-code counters for any peer that has rolling_code: true.
  for (uint8_t i = 0; i < static_peer_count_; i++) {
    ESPNowPeerConfig &cfg = static_peers_[i];
    if (!cfg.rolling_code)
      continue;
    // FNV-1a hash of the 6 MAC bytes — stable, unique per peer.
    uint32_t h = 2166136261UL;
    for (int j = 0; j < 6; j++) { h ^= cfg.mac[j]; h *= 16777619UL; }
    cfg.tx_pref = global_preferences->make_preference<uint32_t>(h ^ 0x455354UL, true);
    cfg.rx_pref = global_preferences->make_preference<uint32_t>(h ^ 0x455352UL, true);
    if (!cfg.tx_pref.load(&cfg.tx_counter))  cfg.tx_counter  = 0;
    if (!cfg.rx_pref.load(&cfg.rx_last_seen)) cfg.rx_last_seen = 0;
    ESP_LOGD(TAG, "rolling-code %02X:%02X:%02X:%02X:%02X:%02X tx=%u rx_last=%u",
             cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5],
             (unsigned) cfg.tx_counter, (unsigned) cfg.rx_last_seen);
  }

  ESP_LOGD(TAG, "ESP-NOW initialized, %u static peer(s)", static_peer_count_);
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void ESPNowComponent::loop() {
  // A runtime wifi.disable action also turns off both interfaces. Restore only
  // when the Wi-Fi component is genuinely disabled; never fight its temporary
  // adapter restart/reconnect sequence.
  const uint32_t now = millis();
  if (now - this->last_radio_check_ >= 1000) {
    this->last_radio_check_ = now;
    bool ready = false;
    if (this->should_manage_radio_()) {
      ready = this->ensure_radio_ready_();
    } else {
#ifdef USE_WIFI
      // ESPHome owns the adapter while enabled. Wait until association is
      // complete so scans/reconnects are not disturbed, then inherit its
      // actual channel and restore legacy peer metadata if necessary.
      ready = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected() &&
              (wifi_get_opmode() & STATION_MODE) != 0;
#endif
    }

    if (ready) {
      if (this->synchronize_runtime_config_())
        this->status_clear_warning();
      else
        this->status_set_warning();
    } else if (this->should_manage_radio_()) {
      this->status_set_warning();
    }
  }

  while (recv_queue_read_ != recv_queue_write_) {
    dispatch_(recv_queue_[recv_queue_read_]);
    recv_queue_read_ = (recv_queue_read_ + 1) % ESPNOW_QUEUE_SIZE;
  }

  if (send_cb_fired_) {
    send_cb_fired_ = false;
    auto cb = pending_send_cb_;
    pending_send_cb_ = nullptr;
    send_pending_ = false;
    ESP_LOGD(TAG, "TX ack: %s", send_cb_status_ == 0 ? "OK" : "FAIL");
    if (cb)
      cb(send_cb_status_ == 0 ? 0 : -1);  // 0 = ESP_NOW_SEND_SUCCESS on ESP8266
    this->start_next_send_();
  }
}

void ESPNowComponent::dump_config() {
  uint8_t station_mac[6]{};
  uint8_t softap_mac[6]{};
  ESP_LOGCONFIG(TAG, "ESP-NOW:");
  ESP_LOGCONFIG(TAG, "  Channel: %u (0 = inherit from wifi)", channel_);
  ESP_LOGCONFIG(TAG, "  Actual radio channel: %u", wifi_get_channel());
  ESP_LOGCONFIG(TAG, "  Role: %s", espnow_role_to_string(this->self_role_));
  ESP_LOGCONFIG(TAG, "  Radio owner: %s", this->should_manage_radio_() ? "ESP-NOW (Wi-Fi networking disabled)"
                                                                       : "ESPHome Wi-Fi");
  ESP_LOGCONFIG(TAG, "  Wi-Fi opmode: %s", wifi_opmode_to_string(wifi_get_opmode()));
  if (wifi_get_macaddr(STATION_IF, station_mac))
    ESP_LOGCONFIG(TAG, "  Station MAC: %02X:%02X:%02X:%02X:%02X:%02X", station_mac[0], station_mac[1],
                  station_mac[2], station_mac[3], station_mac[4], station_mac[5]);
  if (wifi_get_macaddr(SOFTAP_IF, softap_mac))
    ESP_LOGCONFIG(TAG, "  SoftAP MAC: %02X:%02X:%02X:%02X:%02X:%02X", softap_mac[0], softap_mac[1],
                  softap_mac[2], softap_mac[3], softap_mac[4], softap_mac[5]);
  ESP_LOGCONFIG(TAG, "  PMK: %s", has_pmk_ ? "set" : "not set");
  ESP_LOGCONFIG(TAG, "  Auto-add peers: %s", auto_add_peer_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Static peers: %u", static_peer_count_);
  for (uint8_t i = 0; i < static_peer_count_; i++) {
    const uint8_t *m = static_peers_[i].mac;
    ESP_LOGCONFIG(TAG, "    %02X:%02X:%02X:%02X:%02X:%02X (channel %u, role %s)%s%s",
                  m[0], m[1], m[2], m[3], m[4], m[5],
                  this->effective_peer_channel_(static_peers_[i].channel),
                  espnow_role_to_string(static_peers_[i].role),
                  static_peers_[i].encrypt ? " (encrypted)" : "",
                  static_peers_[i].rolling_code ? " (rolling-code)" : "");
  }
}

// ── Peer management ───────────────────────────────────────────────────────────

void ESPNowComponent::set_pmk(const std::array<uint8_t, 16> &pmk) {
  has_pmk_ = true;
  memcpy(pmk_, pmk.data(), 16);
}

void ESPNowComponent::add_static_peer(peer_address_t mac, uint8_t channel, uint8_t role) {
  if (static_peer_count_ >= ESPNOW_MAX_TOTAL_PEER_NUM) {
    ESP_LOGW(TAG, "Maximum peer count (%u) reached", ESPNOW_MAX_TOTAL_PEER_NUM);
    return;
  }
  ESPNowPeerConfig &cfg = static_peers_[static_peer_count_++];
  memcpy(cfg.mac, mac.data(), 6);
  cfg.channel = channel;
  cfg.role = role;
  cfg.encrypt = false;
  memset(cfg.lmk, 0, 16);
}

void ESPNowComponent::add_static_peer_with_lmk(peer_address_t mac, uint8_t channel, uint8_t role,
                                               std::array<uint8_t, 16> lmk) {
  if (static_peer_count_ >= ESPNOW_MAX_TOTAL_PEER_NUM) {
    ESP_LOGW(TAG, "Maximum peer count (%u) reached", ESPNOW_MAX_TOTAL_PEER_NUM);
    return;
  }
  ESPNowPeerConfig &cfg = static_peers_[static_peer_count_++];
  memcpy(cfg.mac, mac.data(), 6);
  cfg.channel = channel;
  cfg.role = role;
  cfg.encrypt = true;
  memcpy(cfg.lmk, lmk.data(), 16);
}

int ESPNowComponent::add_peer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(const_cast<uint8_t *>(mac)))
    return 0;

  const uint8_t peer_channel = this->effective_peer_channel_(0);
  int err = esp_now_add_peer(const_cast<uint8_t *>(mac), ESP_NOW_ROLE_SLAVE, peer_channel, nullptr, 0);
  if (err != 0) {
    ESP_LOGW(TAG, "add_peer %02X:%02X:%02X:%02X:%02X:%02X on channel %u failed: %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], peer_channel, err);
  }
  return err;
}

int ESPNowComponent::del_peer(const uint8_t *mac) {
  int err = esp_now_del_peer(const_cast<uint8_t *>(mac));
  if (err != 0) {
    ESP_LOGW(TAG, "del_peer %02X:%02X:%02X:%02X:%02X:%02X failed: %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], err);
  }
  return err;
}

ESPNowPeerConfig *ESPNowComponent::find_peer_(const uint8_t *mac) {
  for (uint8_t i = 0; i < static_peer_count_; i++) {
    if (memcmp(static_peers_[i].mac, mac, 6) == 0)
      return &static_peers_[i];
  }
  return nullptr;
}

void ESPNowComponent::set_rolling_code(peer_address_t mac) {
  ESPNowPeerConfig *cfg = this->find_peer_(mac.data());
  if (cfg == nullptr) {
    ESP_LOGW(TAG, "set_rolling_code: peer %02X:%02X:%02X:%02X:%02X:%02X not found",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return;
  }
  cfg->rolling_code = true;
}

void ESPNowComponent::reset_rolling_code(const uint8_t *mac) {
  ESPNowPeerConfig *cfg = this->find_peer_(mac);
  if (cfg == nullptr || !cfg->rolling_code) {
    ESP_LOGW(TAG, "reset_rolling_code: peer not found or not configured for rolling-code");
    return;
  }
  cfg->rx_last_seen = 0;
  cfg->rx_pref.save(&cfg->rx_last_seen);
  ESP_LOGI(TAG, "rolling-code RX counter reset for %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── Send ──────────────────────────────────────────────────────────────────────

int ESPNowComponent::send(const uint8_t *peer, const uint8_t *data, size_t size,
                          const send_callback_t &callback) {
  if (this->is_failed())
    return -1;

  if (peer == nullptr || data == nullptr || size == 0) {
    ESP_LOGW(TAG, "Cannot send an empty packet or use a null address");
    return -1;
  }

  if (size > ESPNOW_MAX_DATA_LEN) {
    ESP_LOGW(TAG, "Payload too large (%u > %u bytes)", (unsigned) size, ESPNOW_MAX_DATA_LEN);
    return -1;
  }

  if (!esp_now_is_peer_exist(const_cast<uint8_t *>(peer))) {
    if (auto_add_peer_) {
      this->add_peer(peer);
    } else {
      ESP_LOGW(TAG, "send to unregistered peer %02X:%02X:%02X:%02X:%02X:%02X",
               peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
      return -1;
    }
  }

  ESPNowPeerConfig *cfg = this->find_peer_(peer);
  if (cfg != nullptr && cfg->rolling_code && size + 4 > ESPNOW_MAX_DATA_LEN) {
    ESP_LOGE(TAG, "rolling-code payload too large (%u bytes, max %u for rolling-code peers)",
             (unsigned) size, ESPNOW_MAX_DATA_LEN - 4);
    return -1;
  }

  if (this->send_queue_.size() >= ESPNOW_SEND_QUEUE_SIZE) {
    ESP_LOGW(TAG, "Send queue full (%u packets), dropping len=%u", ESPNOW_SEND_QUEUE_SIZE, (unsigned) size);
    return -1;
  }

  ESPNowSendQueueEntry entry;
  memcpy(entry.address.data(), peer, entry.address.size());
  entry.data.assign(data, data + size);
  entry.callback = callback;
  this->send_queue_.push_back(std::move(entry));
  this->start_next_send_();
  return 0;
}

void ESPNowComponent::start_next_send_() {
  if (this->processing_send_queue_ || this->send_pending_ || this->send_queue_.empty() || this->is_failed())
    return;

  this->processing_send_queue_ = true;
  while (!this->send_pending_ && !this->send_queue_.empty()) {
    ESPNowSendQueueEntry entry = std::move(this->send_queue_.front());
    this->send_queue_.pop_front();

    const uint8_t *data = entry.data.data();
    size_t size = entry.data.size();
    uint8_t rc_frame[ESPNOW_MAX_DATA_LEN];
    ESPNowPeerConfig *cfg = this->find_peer_(entry.address.data());
    if (cfg != nullptr && cfg->rolling_code) {
      cfg->tx_counter++;
      cfg->tx_pref.save(&cfg->tx_counter);
      rc_frame[0] = (cfg->tx_counter >> 24) & 0xFF;
      rc_frame[1] = (cfg->tx_counter >> 16) & 0xFF;
      rc_frame[2] = (cfg->tx_counter >> 8) & 0xFF;
      rc_frame[3] = cfg->tx_counter & 0xFF;
      memcpy(rc_frame + 4, data, size);
      data = rc_frame;
      size += 4;
    }

    pending_send_cb_ = std::move(entry.callback);
    send_cb_fired_ = false;
    send_pending_ = true;

    // esp_now_send copies the buffer before returning, so both entry.data and
    // the rolling-code stack frame remain valid for the complete SDK call.
    int err = esp_now_send(entry.address.data(), const_cast<uint8_t *>(data), (int) size);
    if (err != 0) {
      ESP_LOGW(TAG, "esp_now_send failed: %d", err);
      auto cb = std::move(this->pending_send_cb_);
      this->pending_send_cb_ = nullptr;
      this->send_pending_ = false;
      if (cb)
        cb(err);
      continue;
    }

    ESP_LOGD(TAG, "TX to %02X:%02X:%02X:%02X:%02X:%02X len=%u queued=%u", entry.address[0],
             entry.address[1], entry.address[2], entry.address[3], entry.address[4], entry.address[5],
             (unsigned) size, (unsigned) this->send_queue_.size());
  }
  this->processing_send_queue_ = false;
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void ESPNowComponent::dispatch_(const ESPNowQueueEntry &entry) {
  const uint8_t *src = entry.info.src_addr;

  ESP_LOGD(TAG, "RX from %02X:%02X:%02X:%02X:%02X:%02X len=%u data[0]=%02X data[1]=%02X",
           src[0], src[1], src[2], src[3], src[4], src[5], entry.size,
           entry.size > 0 ? entry.data[0] : 0,
           entry.size > 1 ? entry.data[1] : 0);

  bool peer_known = esp_now_is_peer_exist(const_cast<uint8_t *>(src));
  ESP_LOGD(TAG, "  peer_known=%d receive_handlers=%u broadcast_handlers=%u",
           (int) peer_known, (unsigned) receive_handlers_.size(), (unsigned) broadcast_handlers_.size());

  if (peer_known) {
    // Rolling-code validation: strip and check the 4-byte counter if this peer
    // is configured with rolling_code: true. Non-rolling-code peers skip this block.
    ESPNowPeerConfig *cfg = this->find_peer_(src);
    if (cfg != nullptr && cfg->rolling_code) {
      if (entry.size < 4) {
        ESP_LOGW(TAG, "  rolling-code frame too short (%u bytes), dropping", entry.size);
        return;
      }
      uint32_t counter = ((uint32_t) entry.data[0] << 24) | ((uint32_t) entry.data[1] << 16) |
                         ((uint32_t) entry.data[2] << 8)  |  (uint32_t) entry.data[3];
      if (counter <= cfg->rx_last_seen) {
        ESP_LOGW(TAG, "  rolling-code replay (counter=%u <= last_seen=%u), dropping",
                 (unsigned) counter, (unsigned) cfg->rx_last_seen);
        return;
      }
      cfg->rx_last_seen = counter;
      cfg->rx_pref.save(&cfg->rx_last_seen);
      // Strip the 4-byte counter header before dispatching to handlers.
      const uint8_t *payload = entry.data + 4;
      uint8_t payload_size   = entry.size - 4;
      for (auto *h : receive_handlers_) {
        ESP_LOGD(TAG, "  calling receive_handler (rolling-code counter=%u)", (unsigned) counter);
        if (h->on_receive(entry.info, payload, payload_size))
          break;
      }
      return;
    }
    for (auto *h : receive_handlers_) {
      ESP_LOGD(TAG, "  calling receive_handler");
      if (h->on_receive(entry.info, entry.data, entry.size))
        break;
    }
  } else {
    // On ESP8266, des_addr is not available in the receive callback, so broadcast
    // and unknown-unicast frames are indistinguishable. Frames from unregistered
    // sources fire both on_broadcast and on_unknown_peer.
    for (auto *h : broadcast_handlers_) {
      if (h->on_broadcast(entry.info, entry.data, entry.size))
        break;
    }
    for (auto *h : unknown_peer_handlers_) {
      if (h->on_unknown_peer(entry.info, entry.data, entry.size))
        break;
    }
    if (auto_add_peer_) {
      this->add_peer(src);
    }
  }
}

// ── Static callbacks (WiFi task — must not allocate or call ESPHome APIs) ─────

void ICACHE_RAM_ATTR ESPNowComponent::recv_cb_(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (global_esp_now == nullptr || len == 0 || len > ESPNOW_MAX_DATA_LEN)
    return;

  uint8_t next = (recv_queue_write_ + 1) % ESPNOW_QUEUE_SIZE;
  if (next == global_esp_now->recv_queue_read_)
    return;  // queue full — frame dropped

  ESPNowQueueEntry &entry = recv_queue_[recv_queue_write_];
  memcpy(entry.info.src_addr, mac, 6);
  memcpy(entry.data, data, len);
  entry.size = len;

  recv_queue_write_ = next;
}

void ICACHE_RAM_ATTR ESPNowComponent::send_cb_(uint8_t *mac, uint8_t status) {
  if (global_esp_now == nullptr)
    return;
  send_cb_status_ = status;
  send_cb_fired_ = true;
}

}  // namespace espnow
}  // namespace esphome

#endif  // USE_ESP8266
