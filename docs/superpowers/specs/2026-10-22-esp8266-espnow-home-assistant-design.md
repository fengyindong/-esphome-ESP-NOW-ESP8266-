# ESP8266 ESP-NOW Home Assistant 传感器设计

## 目标

使用两块基于 ESP8266-12E 的 NodeMCU 开发板构建一条 ESP-NOW 单播链路。持续供电的从机每 30 秒读取共用 I2C 总线上的 AHT20 与 BMP280，并通过 `packet_transport` 将测量值发送给主机。主机连接 2.4 GHz Wi-Fi，并通过 ESPHome Native API 将远程传感器接入 Home Assistant。

## 版本与硬件约束

- ESPHome 固定使用 `2026.8.0`，与外部组件仓库声明的 `packet_transport` 实现版本一致。
- 主机和从机均为 NodeMCU ESP8266-12E，ESPHome board 使用 `nodemcuv2`。
- AHT20 与 BMP280 共用从机 I2C 总线：SDA 为 GPIO4（D2），SCL 为 GPIO5（D1）。
- AHT20 地址为 `0x38`。BMP280 默认地址为 `0x76`；若 I2C 扫描显示 `0x77`，只修改从机的 `bmp280_address` substitution。
- 2.4 GHz 路由器固定在信道 6。主机的无线信道由已连接的 Wi-Fi 接入点控制；从机未配置 Wi-Fi，必须显式使用 ESP-NOW 信道 6。
- 两端使用 Station MAC 互相注册为 peer，不使用 SoftAP MAC。两端本机和 peer 均使用 ESP8266 legacy `COMBO` 角色，使两个节点都具备收发能力。

## 文件结构

- `mac-discovery.yaml`：可分别烧录到两块板，通过串口日志读取 Station MAC。
- `espnow-sensor-node.yaml`：从机传感器、ESP-NOW peer 与 `packet_transport` provider 配置。
- `espnow-gateway.yaml`：主机 Wi-Fi、Home Assistant API、ESP-NOW peer 与远程传感器配置。
- `secrets.yaml.example`：Wi-Fi、API 和 OTA 所需密钥的示例，不包含真实凭据。
- `README.md`：接线、版本固定、MAC 发现、配置替换、烧录、调试和验收说明。
- `components/`：基于固定上游提交的本地组件副本，包含 ESP8266 双端 `COMBO` 角色扩展。

## 组件来源

`components/` 基于以下仓库的固定提交：

`https://github.com/gasment/esphome_espnow_for_esp8266_with_packet_transport`

基础版本为提交 `ce552856127f91371f90ddb40e397536e46f1531`。由于该提交将本机角色固定为 `CONTROLLER`、peer 固定为 `SLAVE`，且 README 明确说明 ESP8266 与 ESP8266 尚未测试，本项目在本地副本中增加可配置的 `role`，两端均配置为 `COMBO`。YAML 通过 `external_components` 的 local source 加载该副本，避免上游变化和运行时下载影响构建。

## 设备配置

### MAC 发现配置

发现配置不连接 Wi-Fi，启用串口 logger，并以信道 6 初始化 `espnow`。组件启动日志必须显示 `Station MAC` 和实际无线信道。用户先后将该配置烧录到两块板并记录 MAC，再将它们填入正式配置的 substitutions。

### 从机

设备名为 `espnow-sensor-node`。从机不配置 `wifi`、`api`、`ota` 或回退热点，只保留串口日志和 ESP-NOW；这样无线电始终停留在信道 6。

传感器每 30 秒更新一次：

| 广播 ID | 来源 | 单位 | 用途 |
| --- | --- | --- | --- |
| `aht20_temperature` | AHT20 | 摄氏度 | 主要环境温度 |
| `aht20_humidity` | AHT20 | 百分比 | 相对湿度 |
| `bmp280_temperature` | BMP280 | 摄氏度 | 补偿温度与交叉校验 |
| `bmp280_pressure` | BMP280 | hPa | 大气压强 |

`packet_transport` 使用 `platform: espnow`，`peer_address` 为主机 Station MAC，`update_interval` 为 30 秒，并将以上四个本地 sensor ID 作为 provider 数据发送。ESP-NOW peer 列表只登记主机 Station MAC，禁用自动添加 peer 和 rolling code。

### 主机

设备名为 `espnow-gateway`。主机使用 `secrets.yaml` 连接固定信道 6 的 2.4 GHz Wi-Fi，通过加密的 ESPHome Native API 接入 Home Assistant，并启用带密码的 OTA。

主机的 `espnow` 不设置 `channel`，因为接入点连接后实际信道由 Wi-Fi 管理；peer channel 使用 `0` 跟随当前无线信道。peer 列表只登记从机 Station MAC，禁用自动添加 peer 和 rolling code。

主机 `packet_transport` 注册 provider `espnow-sensor-node`。四个远程 sensor 分别使用与从机广播 ID 完全相同的 `remote_id`，并设置合适的 device class、state class、单位与显示精度。只有主机侧实体暴露给 Home Assistant；从机本地传感器标记为 internal。

## 数据流

1. AHT20 和 BMP280 在从机上每 30 秒发布本地状态。
2. 从机的 `packet_transport` 将设备名、字段 ID 和浮点值编码到不超过 250 字节的数据包。
3. ESP-NOW 将数据包单播至主机 Station MAC。
4. 主机按来源 peer、provider 名和 `remote_id` 解码并更新远程传感器。
5. ESPHome Native API 将新状态推送给 Home Assistant。

## 安全与可靠性

- Home Assistant API 使用 Noise 加密密钥，OTA 使用独立密码；真实值仅保存在未提交的 `secrets.yaml`。
- 第一阶段不启用 ESP-NOW PMK/LMK、XXTEA、rolling code 或 ping-pong，以减少联调变量。
- 单播发送失败、未知 peer、组件初始化失败和实际信道均由 logger 输出。
- 启用 I2C 扫描，启动时应发现 `0x38` 与 `0x76` 或 `0x77`。
- 路由器不得自动改变 2.4 GHz 信道；变更路由器信道时必须同步修改从机配置并重新烧录。
- 主机离线时从机会继续定时尝试发送；恢复主机后无需重新配对。

## 验证标准

### 静态验证

- 使用 ESPHome `2026.8.0` 对发现配置、从机配置和主机配置执行配置校验。
- 使用测试 secrets 和合法测试 MAC 编译两份正式配置。
- 确认编译日志加载固定提交中的 `espnow` 与 `packet_transport`，目标平台为 ESP8266 Arduino。

### 硬件验收

1. 两块板的发现日志均显示唯一的 Station MAC，实际信道为 6。
2. 从机 I2C 扫描发现 AHT20 和 BMP280，四项测量值合理且无 `nan`。
3. 主机连接路由器后日志显示实际信道为 6，并成功注册从机 peer。
4. 从机日志不持续出现 ESP-NOW send failure；主机日志能看到来自从机 Station MAC 的数据包。
5. Home Assistant 自动发现 `espnow-gateway`，四个实体至少每 30 秒左右刷新一次。
6. 主机断电再恢复后，数据上报自动恢复，不需要重新烧录或重新配对。

## 后续范围

基础链路稳定后，可另行设计从机深度睡眠与电池供电。该阶段需要将 GPIO16（D0）连接到 RST，并重新评估首次采样等待时间、发送完成确认和唤醒周期。本次实现不包含深度睡眠、ESP-NOW 加密或多从机支持。
