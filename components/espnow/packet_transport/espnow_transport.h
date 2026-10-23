#pragma once

#include "../espnow_component.h"

#ifdef USE_ESP8266

#include "esphome/core/component.h"
#include "esphome/components/packet_transport/packet_transport.h"

#include <deque>
#include <vector>

namespace esphome::espnow {

class ESPNowTransport final : public packet_transport::PacketTransport,
                              public Parented<ESPNowComponent>,
                              public ESPNowReceivePacketHandler,
                              public ESPNowBroadcastHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_peer_address(peer_address_t address) { this->peer_address_ = address; }

  // ESPNow handler interface
  bool on_receive(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override;
  bool on_broadcast(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override;

 protected:
  void send_packet(const std::vector<uint8_t> &buf) const override;
  void send_next_() const;
  size_t get_max_packet_size() override { return ESPNOW_MAX_DATA_LEN; }
  bool should_send() override;

  peer_address_t peer_address_{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
  std::vector<uint8_t> packet_buffer_;
  static constexpr size_t TX_QUEUE_LIMIT = 8;
  mutable std::deque<std::vector<uint8_t>> tx_queue_;
  mutable bool tx_in_flight_{false};
};

}  // namespace esphome::espnow

#endif  // USE_ESP8266
