# ESP8266 ESP-NOW Home Assistant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and verify ESPHome configurations that send AHT20 and BMP280 measurements from an ESP8266 sensor node to an ESP8266 Home Assistant gateway over ESP-NOW.

**Architecture:** The sensor node stays off Wi-Fi on fixed channel 6 and publishes four internal sensor values through the external component's ESP-NOW `packet_transport`. The gateway follows its Wi-Fi access point on channel 6, decodes the named provider fields, and exposes them through the encrypted ESPHome Native API.

**Tech Stack:** ESPHome 2026.8.0, ESP8266 Arduino framework, YAML, Python `unittest`, external `espnow` and `packet_transport` components.

**Spec:** `docs/superpowers/specs/2026-10-22-esp8266-espnow-home-assistant-design.md`

## Global Constraints

- Use ESPHome `2026.8.0` from a project-local `.venv`.
- Use NodeMCU ESP8266-12E board identifier `nodemcuv2` for both devices.
- Use GPIO4 for SDA and GPIO5 for SCL on the sensor node.
- Keep the router and Wi-Fi network fixed to 2.4 GHz channel 6.
- Use only Station MAC addresses for ESP-NOW peers.
- Pin the external component to commit `ce552856127f91371f90ddb40e397536e46f1531`.
- Do not enable ESP-NOW encryption, rolling code, ping-pong, deep sleep, or multi-node support.
- Never commit a real `secrets.yaml`.

---

### Task 1: Configuration Contract Tests and Project Hygiene

**Files:**
- Create: `.gitignore`
- Create: `tests/test_configuration.py`

**Interfaces:**
- Consumes: approved design document.
- Produces: executable structural contracts for all YAML deliverables.

- [ ] **Step 1: Write failing configuration tests**

Create `tests/test_configuration.py` using Python `unittest`. Each test invokes the real ESPHome `config` command and fails unless ESPHome accepts the complete configuration, including external component schemas and cross-component references. The first run must fail because the YAML deliverables do not exist.

```python
class ConfigurationContractTests(unittest.TestCase):
    def assert_esphome_accepts(self, filename: str) -> None:
        completed = subprocess.run(
            [sys.executable, "-m", "esphome", "config", filename],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_esphome_accepts_sensor_node_configuration(self):
        self.assert_esphome_accepts("espnow-sensor-node.yaml")
```

- [ ] **Step 2: Run tests and verify RED**

Run: `python -m unittest discover -s tests -v`

Expected: FAIL because `mac-discovery.yaml`, `espnow-sensor-node.yaml`, and `espnow-gateway.yaml` do not exist.

- [ ] **Step 3: Add project hygiene**

Create `.gitignore` with `.venv/`, `.esphome/`, `secrets.yaml`, Python caches, and test output excluded.

- [ ] **Step 4: Run the tests again**

Run: `python -m unittest discover -s tests -v`

Expected: the same expected missing-deliverable failures; `.gitignore` does not weaken the contract.

- [ ] **Step 5: Commit**

```powershell
git add .gitignore tests/test_configuration.py
git commit -m "test: define ESP-NOW configuration contracts"
```

---

### Task 2: MAC Discovery and Sensor Node

**Files:**
- Create: `mac-discovery.yaml`
- Create: `espnow-sensor-node.yaml`

**Interfaces:**
- Consumes: `gateway_mac` substitution containing the gateway Station MAC.
- Produces: provider `espnow-sensor-node` with fields `aht20_temperature`, `aht20_humidity`, `bmp280_temperature`, and `bmp280_pressure` every 30 seconds.

- [ ] **Step 1: Create MAC discovery configuration**

Use ESP8266 `nodemcuv2`, serial logger, pinned external `espnow` component, and `espnow.channel: 6`. Do not configure Wi-Fi.

- [ ] **Step 2: Create minimal sensor node configuration**

Configure substitutions for `gateway_mac` and `bmp280_address`, I2C scanning on GPIO4/GPIO5, AHT20 through `platform: aht10` with `variant: AHT20`, BMP280 through `platform: bmp280_i2c`, and four internal sensor IDs. Configure the pinned external components, a static gateway peer, and ESP-NOW packet transport:

```yaml
packet_transport:
  - platform: espnow
    id: sensor_transport
    espnow_id: espnow_bus
    peer_address: ${gateway_mac}
    update_interval: 30s
    sensors:
      - id: aht20_temperature
        broadcast_id: aht20_temperature
      - id: aht20_humidity
        broadcast_id: aht20_humidity
      - id: bmp280_temperature
        broadcast_id: bmp280_temperature
      - id: bmp280_pressure
        broadcast_id: bmp280_pressure
```

- [ ] **Step 3: Run targeted tests**

Run: `python -m unittest tests.test_configuration.ConfigurationContractTests.test_mac_discovery_contract tests.test_configuration.ConfigurationContractTests.test_sensor_node_contract -v`

Expected: PASS.

- [ ] **Step 4: Commit**

```powershell
git add mac-discovery.yaml espnow-sensor-node.yaml
git commit -m "feat: add ESP-NOW sensor node configuration"
```

---

### Task 3: Home Assistant Gateway and Deployment Guide

**Files:**
- Create: `espnow-gateway.yaml`
- Create: `secrets.yaml.example`
- Create: `README.md`

**Interfaces:**
- Consumes: provider `espnow-sensor-node`, the four exact remote IDs, `sensor_node_mac`, Wi-Fi credentials, API encryption key, and OTA password.
- Produces: four Home Assistant sensor entities on device `espnow-gateway`.

- [ ] **Step 1: Create gateway configuration**

Configure Wi-Fi and encrypted Native API from secrets, OTA, logger, pinned external components, a static sensor-node peer with inherited channel `0`, and one ESP-NOW packet transport receiver. Add four `platform: packet_transport` sensors with exact provider and remote IDs.

```yaml
sensor:
  - platform: packet_transport
    transport_id: gateway_transport
    provider: espnow-sensor-node
    remote_id: aht20_temperature
    name: "AHT20 Temperature"
    device_class: temperature
    state_class: measurement
    unit_of_measurement: "°C"
```

- [ ] **Step 2: Create example secrets**

Document `wifi_ssid`, `wifi_password`, `api_encryption_key`, and `ota_password` with safe example values. Real credentials remain in ignored `secrets.yaml`.

- [ ] **Step 3: Write deployment guide**

Document wiring, ESPHome version setup, router channel, MAC discovery, substitutions, compile and upload order, expected logs, Home Assistant onboarding, BMP280 address correction, and troubleshooting.

- [ ] **Step 4: Run all structural tests**

Run: `python -m unittest discover -s tests -v`

Expected: PASS with all configuration contracts satisfied.

- [ ] **Step 5: Commit**

```powershell
git add espnow-gateway.yaml secrets.yaml.example README.md
git commit -m "feat: add ESP-NOW Home Assistant gateway"
```

---

### Task 4: ESPHome 2026.8.0 Validation and Compilation

**Files:**
- Modify only if validation finds a defect: `mac-discovery.yaml`, `espnow-sensor-node.yaml`, `espnow-gateway.yaml`, `tests/test_configuration.py`, `README.md`
- Create locally but do not commit: `.venv/`, `secrets.yaml`, `.esphome/`

**Interfaces:**
- Consumes: all YAML configurations and safe test credentials/MACs.
- Produces: verified ESP8266 firmware builds and documented reproducible commands.

- [ ] **Step 1: Create the isolated tool environment**

Run:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install "esphome==2026.8.0"
.\.venv\Scripts\python.exe -m esphome version
```

Expected: `Version: 2026.8.0`.

- [ ] **Step 2: Create ignored test secrets**

Create `secrets.yaml` locally with syntactically valid dummy Wi-Fi values, a valid 32-byte base64 API key, and an OTA password.

- [ ] **Step 3: Validate every YAML file**

Run:

```powershell
.\.venv\Scripts\python.exe -m esphome config mac-discovery.yaml
.\.venv\Scripts\python.exe -m esphome config espnow-sensor-node.yaml
.\.venv\Scripts\python.exe -m esphome config espnow-gateway.yaml
```

Expected: all three commands exit 0 with no schema errors.

- [ ] **Step 4: Compile all firmware configurations**

Run the same three configurations with `compile` instead of `config`.

Expected: all builds finish with `SUCCESS`; generated firmware targets ESP8266 `nodemcuv2` and loads the pinned external component commit.

- [ ] **Step 5: Run final checks**

Run:

```powershell
python -m unittest discover -s tests -v
git diff --check
git status --short
```

Expected: tests PASS, no whitespace errors, and only the ignored local test artifacts remain outside Git.

- [ ] **Step 6: Commit validation fixes if needed**

```powershell
git add mac-discovery.yaml espnow-sensor-node.yaml espnow-gateway.yaml tests/test_configuration.py README.md
git commit -m "fix: validate ESP-NOW configs with ESPHome 2026.8.0"
```

Skip this commit when validation required no tracked-file changes.
