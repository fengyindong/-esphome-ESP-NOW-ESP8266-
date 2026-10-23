# ESP8266 ESP-NOW Packet Transport

This platform adapts ESPHome's `packet_transport` component to the legacy
ESP8266 Non-OS ESP-NOW API.

## Interface address

The parent ESP-NOW component exposes the legacy local and peer roles through
the YAML `role` option. The default remains local `controller` with `slave`
peers for compatibility with the upstream implementation. ESP8266-to-ESP8266
links should configure both local devices and their peer entries as `combo`,
which allows both endpoints to transmit and receive. The startup log prints
the selected role, Wi-Fi opmode, and both Station and SoftAP MACs; the peer
addresses in this project use the Station MACs.

With this project connected as a normal Wi-Fi station, captured ESP-NOW action
frames use the Station MAC. SoftAP beacon frames may still be visible when the
configured fallback AP is active; beacon source addresses do not identify the
ESP-NOW data interface.

## Sending

The ESP8266 SDK permits one outstanding ESP-NOW send. Packet Transport may
produce several packets in one update, so this adapter keeps a bounded FIFO
and sends the next packet only after the parent component reports completion.
The example below uses unicast, so a successful send callback confirms MAC-layer
delivery to the configured gateway peer.

```yaml
packet_transport:
  - platform: espnow
    peer_address: 3C:DC:75:72:3C:04
    sensors:
      - example_sensor
```
