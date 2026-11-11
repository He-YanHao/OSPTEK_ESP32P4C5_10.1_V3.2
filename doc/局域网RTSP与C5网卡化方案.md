# 局域网 RTSP 与 C5 网卡化方案 · 本工程适配评估

> **状态：方案评估稿（2026-11-11）**。用户明确：Matter-WebRTC 暂缓，先做**局域网 RTSP**；并指出当前 C5 跑的 ESP-AT SDIO 固件不合适，希望「直接把 C5 当网卡使用」。
> 本文只做事实核对与路径设计，**尚未烧录 / 尚未跑通**。摄像头裸显阶段（色块、闪烁已解决，偏绿发暗按用户要求不再追）见 `doc/摄像头裸显预览方案.md` 第 9 节。

## 0. 结论摘要

| 项 | 结论 |
| --- | --- |
| 推荐方案 | 把 C5 从 **ESP-AT SDIO 固件**换成 **ESP-Hosted 3.x 的 co-processor 固件（`cp/`）**，P4 侧得到标准 `lwIP / esp_netif / socket`，即真正的网卡 |
| 硬件是否需要改 | **不需要**。已按核心板手册原图核实：P4↔C5 就是 4-bit SDIO，且引脚与 ESP-Hosted 官方支持的 C5/P4 组合逐脚吻合 |
| IDF 兼容性 | ✅ `espressif/esp_hosted` **3.1.0** 要求 `idf >= 5.5`，targets 含 `esp32c5` 与 `esp32p4`；本工程 v6.1 在范围内 |
| RTSP 栈 | ✅ 官方组件齐备：`esp_video`（采集，默认传感器就是 SC2336）+ `esp_h264`（P4 硬件编码）+ `esp_rtsp_service`（RTSP Server） |
| 主要工作量 | ① 重刷 C5 固件并对齐 SDIO 配置；② ISP 输出从 RGB565 改到 YUV420 供 H.264；③ 重规划 768K app 分区 |
| 建议 | **先在独立验证工程跑通 S1（C5 网卡化）再动本工程基线**，与之前 Matter 评估的结论一致 |

顺带关掉了 `doc/Matter-WebRTC摄像头方案适配评估.md` 里两个 P0 阻塞：

- 「P4↔C5 物理通道未核实」→ **已核实为 SDIO，引脚见第 3 节**。
- 「C5 固件形态不兼容」→ **有官方 C5 支持的替代固件**（`esp_hosted` 的 `cp/`），不必依赖官方 Matter 示例的 C6 板卡配置。

## 1. 为什么现在的 AT SDIO 固件不合适

现状（见笔记库 `鱼鹰P4C5-AT.md` 与本工程 `components/esp32at_sdio/` 空占位）：C5 跑预编译 AT 合并包 `ESP32C5_2.12.9_0X0.bin`，P4 通过 SDIO 收发 **AT 指令**。

- AT 是**命令式**接口：socket 由 C5 侧的 AT 固件持有，P4 侧只能用 `AT+CIPSTART` / `AT+CIPSEND` 之类指令间接收发，**P4 自己没有网络接口、没有 lwIP socket**。
- RTSP 需要自主控制 TCP 控制连接（OPTIONS/DESCRIBE/SETUP/PLAY）和 RTP/UDP 包序、时间戳、丢包策略，还要与编码器的 NAL 输出按帧节奏配合；AT 的数据通道很难承载这种细粒度控制，吞吐也是瓶颈。
- `esp_wifi_remote` 仓库里确实有 `wifi_remote_over_at` 后端，能让 AT 固件对外呈现 `esp_wifi` API，但官方 README 明确把 **esp-hosted 列为推荐后端**（口径：TCP 吞吐可达 50 Mbps），AT 后端属于低改动、低性能的过渡选项。

因此「更优秀的方案」是：**让 C5 只做 802.11 射频/MAC 卸载，把网络帧交给 P4，IP 栈与 socket 全部跑在 P4 上**。

## 2. 目标形态：C5 = 网卡（ESP-Hosted）

### 2.1 三条路径对比

| 维度 | 现状：AT SDIO | 过渡：`wifi_remote_over_at` | **推荐：`esp_hosted` 3.x** |
| --- | --- | --- | --- |
| C5 固件 | ESP-AT 合并包 | 不变 | 重刷 `esp-hosted-mcu` 的 `cp/` |
| P4 侧接口 | AT 命令 | 标准 `esp_wifi` API（后端仍是 AT） | 标准 `esp_wifi` API + **`lwIP / esp_netif / socket`** |
| IP 栈位置 | C5 | 待实测确认 | **P4（host）** |
| 总线内容 | AT 文本命令 | AT 文本命令 | `eh_frame` 封装的网络帧 + protobuf RPC（**不是 AT 命令**） |
| 吞吐档次 | 低 | 低~中 | 官方口径 TCP 约 50 Mbps |
| 对 RTSP 的适配 | 差 | 中 | 好（标准 socket 直接可用） |
| 改动量 | — | 小 | 中（重刷 C5 + 加组件 + 配引脚） |

> `esp_wifi_remote` 与 `esp_hosted` 3.x 的关系：前者 README 推荐后者作后端，但 `esp-hosted-mcu` 3.x 文档自称「fully-compatible ESP-IDF APIs」且未提 `esp_wifi_remote`。**两者是否需要同时引入，待 S1 实测确认**，本文不臆断。主推 `esp_hosted` 自带的 API 兼容层。

### 2.2 官方依据与版本

- 组件发布名 `espressif/esp_hosted`，当前 **3.1.0**；仓库名 `esp-hosted-mcu`（旧 2.x 的 `slave/` 工程在 3.x 改名为 **`cp/`**）。
- `idf_component.yml`：`idf >= 5.5`；`targets` 明确包含 `esp32c5`、`esp32p4`；无条件依赖 `espressif/esp_tinyusb >= 2.2.0`。
- ESP32-C5 是官方列出的 **SDIO co-processor**（SDIO 仅 ESP32 / C5 / C6 / C61 支持，且使用芯片固定 GPIO）。
- MCU host 侧：SDIO / SPI Full-Duplex / SPI Half-Duplex / UART 均支持；P4 作为 host，Slot 0 固定 GPIO、Slot 1 灵活 GPIO，默认 Slot 1。
- 官方 Getting Started 明确：**host 通过标准 TCP/IP 栈（`lwIP / esp_netif`：DHCP、sockets、ping、iperf）收发网络流量，data path 不需要任何额外 ESP-Hosted API**；control path 才是 `esp_wifi` API → RPC → CP。这正是「把 C5 当网卡」的准确口径。

### 2.3 需要刷的两套固件

ESP-Hosted 是**双固件**方案，两侧必须配置一致：

| 侧 | 工程 | target | 关键配置 |
| --- | --- | --- | --- |
| C5（co-processor） | `examples/wifi/sta/cp/` | `esp32c5` | Transport = SDIO；SDIO 固定 GPIO；bus width / packet-streaming mode |
| P4（host） | `examples/wifi/sta/mcu_host/` 或本工程接入组件 | `esp32p4` | Transport = SDIO；**slave chipset 选 ESP32-C5**；SDIO GPIO 按本板填；bus width / 时钟 / mode 与 C5 一致 |

官方硬规则：**两侧 transport、GPIO、SDIO bus width、Packet vs Streaming mode 必须完全一致**；RPC version 严格匹配，不匹配会 abort 固件。

## 3. 板级硬件核实（已看手册原图）

### 3.1 结论：P4↔C5 是 SDIO + IO

`ESP32-P4C5核心板OSPTEK.pdf` 第 5 页原文：

> 内部连接（ESP32-C5 ↔ ESP32-P4）：ESP32-C5 与 ESP32-P4 之间通过 **SDIO + IO** 互联通信。

同页连接图逐脚记录如下（与笔记库 `鱼鹰P4C5-AT.md` 的表一致）：

| 信号（SDIO 视角） | ESP32-C5 | ESP32-P4 | 手册标注 |
| --- | --- | --- | --- |
| CLK | GPIO9 | GPIO18 | `SDIO_CLK_GPIO18` |
| CMD | GPIO10 | GPIO19 | `SDIO_CMD_GPIO19` |
| DAT0 | GPIO8 | GPIO14 | `SDIO_D0_GPIO14` |
| DAT1 | GPIO7 | GPIO15 | `SDIO_D1_GPIO15` |
| DAT2 | GPIO14 | GPIO16 | `SDIO_D2_GPIO16` |
| DAT3 | GPIO13 | GPIO17 | `SDIO_D3_GPIO17` |
| 额外 IO | GPIO2 | GPIO6 | 手册未标信号名 |
| 复位 | `C5CHIP_PU` | GPIO54 | host 输出 |

### 3.2 与 ESP-Hosted 官方引脚的吻合度

官方 `esp-hosted-mcu` Getting Started 给出的 **ESP32-C5 co-processor SDIO 固定 GPIO**：`CLK=9, CMD=10, D0=8, D1=7, D2=14, D3=13` —— 与本板 C5 侧**逐脚完全一致**。

官方给出的 **ESP32-P4 host（配 C6 的那组示例）**：`CLK=18, CMD=19, D0=14, D1=15, D2=16, D3=17, Reset=54` —— 与本板 P4 侧**逐脚完全一致**。

> 注意官方文档另有一组「P4 host + C5 CP」引脚（`33/4/20/23/21/22`, Reset=53），那是 ESP32-P4-Function-EV-Board 的布局，**不适用于本板**。本板应照 3.1 节填。

**结论：换固件不需要改接线**，只需 host 侧 menuconfig 选 `slave chipset = ESP32-C5` 并把 SDIO GPIO 填成 `CLK18 / CMD19 / D0~D3 = 14/15/16/17`、`Reset = 54`。

### 3.3 口径冲突已澄清

笔记里那条「核心板引脚表 `SD1_*` 是 GPIO39~48，与 AT 表 P4 侧 GPIO14~19 口径不同」的警告，现已明确：

- `ESP32-P4C5-Core核心板_引脚定义.pdf` 中的 `SD1_CDATA0~7 / SD1_CCLK / SD1_CCMD`（GPIO39~48）是**核心板 88 Pin 邮票孔对外引出的 P4 SDIO1 控制器复用脚**，供底板外接 SD 卡 / RMII 等使用。
- **P4↔C5 芯片间内部连接**走的是另一组 GPIO14~19，两者不是同一条总线，不冲突。

### 3.4 仍待实测的硬件项（不臆断）

- 官方要求 `CMD`、`DAT0~DAT3` 全部加 **51 kΩ 外部上拉**（1-bit 模式也不能省），信号为 **3.3 V**。AT 固件能正常通信说明板级大概率已具备，但换固件前建议实测 / 查核心板原理图确认。
- SDIO 起手时钟先用 **400 kHz ~ 5 MHz** 验证握手，稳定后再提高（host 验证范围 400 kHz~20 MHz，SDIO 上限 50 MHz）。
- `C5 GPIO2 ↔ P4 GPIO6` 这根额外 IO 在 ESP-Hosted 里对应 handshake / data-ready / reset 中的哪一个，需对照 `cp/` 与 host 的 menuconfig 确认并两侧对齐。

## 4. C5 固件更换步骤（S1，建议先在独立工程做）

> ⚠️ 会覆盖 C5 现有 AT 固件，**先备份**，且需要物理拨码开关切换烧录目标。

1. **备份现有固件**：拨码切到 C5，用 `esptool.py -p <C5_PORT> read_flash 0x0 <实际 flash 大小> at_backup.bin` 读回保存。
2. **准备工具链**：克隆 `esp-hosted-mcu`（组件目录名建议 `esp_hosted`），执行仓库自带 `./install.sh` / `. ./export.sh`（fish 用 `install.fish` / `export.fish`）。官方说明这两个脚本自带 ESP-IDF 安装与环境配置，**没有单独的 export 步骤**。
   - 本工程主 IDF 是 v6.1，符合 `>=5.5`；但 `install.sh` 自带的 IDF 版本需确认，避免与现有工程环境混用。
3. **构建并烧 C5 的 `cp/`**：
   ```
   cd examples/wifi/sta/cp
   eh.py set-target esp32c5
   eh.py menuconfig     # Transport = SDIO；核对 C5 固定 GPIO、bus width、packet/streaming
   eh.py -p <C5_PORT> flash monitor
   ```
4. **构建 P4 host 验证工程**（先用官方 `mcu_host`，不动本工程）：
   ```
   cd ../mcu_host
   eh.py set-target esp32p4
   eh.py menuconfig     # Transport = SDIO；Slave chipset = ESP32-C5；GPIO 18/19/14~17；Reset 54；AP SSID/密码
   eh.py -p <P4_PORT> flash monitor
   ```
5. **验收判据**（host 串口）：
   ```
   I (...) transport: Received INIT event from ESP32 peripheral
   I (...) transport: Base transport is set-up
   I (...) wifi station: got ip:192.168.x.x
   ```
   C5 侧应打印 `Transport used :: SDIO` / `WLAN over SDIO`。
   **没有 INIT event 就先查接线与 transport 配置，不要去查 Wi-Fi 功能。**
6. **再验网络能力**：host 侧 ping 网关 / iperf / 建一个 TCP+UDP socket，确认 P4 真的拿到了网络接口。

## 5. 局域网 RTSP 实现路径

### 5.1 目标数据流

```text
SC2336 800x800 RAW8
   → MIPI-CSI (bypass)
   → ISP（输出改为 YUV420）
   → ┬→ PPA/转换 → RGB565 → DPI framebuffer（本机预览，可选）
     └→ esp_h264 硬件编码 → Annex-B H.264 NAL
          → esp_rtsp_service (Server, RTP/UDP 或 TCP interleaved)
          → lwIP socket → esp_hosted → C5 Wi-Fi 6 → 局域网
          → VLC / ffplay: rtsp://<P4_IP>:8554/live
```

### 5.2 可用官方组件

| 组件 | 版本 / 要求 | 作用 |
| --- | --- | --- |
| `espressif/esp_hosted` | 3.1.0, `idf>=5.5` | C5 网卡化，P4 拿 lwIP/socket |
| `espressif/esp_video` | v2.3.0, 支持 ESP32-P4 | V4L2 风格：`/dev/video0` MIPI-CSI 采集、`/dev/video11` H.264 编码、`/dev/video20` ISP；**官方示例默认传感器即 SC2336** |
| `espressif/esp_h264` | P4 硬件编码 | ≤1080p 1920×1080@30fps，Baseline，ROI / 码率控制；`esp_h264_enc_hw_new/open/process/close`；输入 `ESP_H264_RAW_FMT_O_UYY_E_VYY`（YUV420） |
| `espressif/esp_rtsp_service` | v0.5.1, IDF release/v5.5+ | RTSP **Server** / Pusher / Puller；H264 或 MJPEG；UDP 与 TCP；`ESP_RTSP_SERVICE_ROLE_SERVER` + `esp_media_service_link` + `esp_service_start` |

另有可参考的现成实践：ESP-ADF `examples/protocols/esp-rtsp`（旧 `esp_rtsp`），以及社区「ESP32-P4 + SC2336 720p + SDIO 接 C6 做 RTSP」案例（实测端到端延时约 110~220 ms），与本工程形态几乎一致。

### 5.3 最大改造点：ISP 输出格式

当前 `components/camera_preview/` 的 ISP 配置是 `output_data_color_type = ISP_COLOR_RGB565`，由 DMA 直写 DPI 帧缓冲；而 H.264 硬件编码器需要 **YUV420**。两者不兼容，必须二选一或加转换：

| 方案 | 做法 | 代价 |
| --- | --- | --- |
| A（最快验证 RTSP） | 暂时关掉本机屏幕预览，ISP 直接输出 YUV420 给编码器 | 屏幕上没有预览；但用户已明确「只要有正确的影像就行」，可接受 |
| B（预览 + 编码并存） | ISP 输出 YUV420，一路给 H.264，另一路经 PPA / 软件转 RGB565 写 DPI fb | 多一次格式转换与带宽占用，需评估 30fps 下的开销 |
| C（走官方标准链路） | 用 `esp_video` V4L2 重写采集，接 `/dev/video11` 编码器 | 最贴合官方示例，但现有 `camera_preview` 要重做 |

**推荐先 A 跑通，再决定 B / C**。三种都要标注为「待选」，未实测前不要写死。

### 5.4 与现有显示链的关系

用户已确认摄像头裸显「色块没有了、也不怎么闪烁了」，偏绿发暗**不再优化**。RTSP 阶段可以复用这条已验证的 CSI/ISP 采集链，只改输出格式与去向；`camera_preview` 里的双缓冲提交、WBG/gamma/color 等显示侧成果保留，不因换方案丢弃。

## 6. 分区与构建约束（必须处理）

- **分区**：`partitions.csv` 的 `factory` 只有 **768K**；GUI 基线镜像已达 752 KiB（占 98%），当前 CAMERA_RAW 是 0x4bdc0。加入 `esp_hosted`（含 `esp_tinyusb`）、`esp_video`、`esp_h264`、`esp_rtsp_service` 与 Wi-Fi/lwIP 栈后**必然超限**。用户已明确允许重分区 → 建议把 `factory` 扩到 **3~4 MB**（从 `storage` 15M 中划），或改双 OTA 分区。**这一步要在 S2 之前做**。
- **MINIMAL_BUILD ON**：新增组件必须显式写进 `main/CMakeLists.txt` 的 `REQUIRES`；模式切换后仍需 `idf.py reconfigure`（既有坑，见 `AGENTS.md`）。
- **组件依赖**：`esp_rtsp_service` 依赖 `esp_media_service` / `esp_service_scheduler` 等 ADF 体系服务，与现有 `lvgl` / `esp_cam_sensor` / `esp_lvgl_adapter` 的兼容性**待实测**。
- `managed_components/**` 禁手改；改动经 `idf_component.yml` 走组件管理器。

## 7. 分步落地计划

| 阶段 | 内容 | 验收判据 | 是否动本工程基线 |
| --- | --- | --- | --- |
| **S0** | 核实硬件通道、组件版本、RTSP 栈（本文） | 一份可执行的引脚/版本清单 | 否（已完成） |
| **S1** | C5 刷 `cp/`，P4 跑官方 `mcu_host` Wi-Fi Station | `Received INIT event` + `Base transport is set-up` + `got ip` + ping/iperf | **否**，独立验证工程 |
| **S2** | 本工程接入 `esp_hosted`，配 SDIO GPIO 18/19/14~17，重规划分区 | P4 工程内 `socket()` / ping 通 | 是 |
| **S3** | 接 `esp_video` + `esp_h264`，SC2336 → YUV420 → H.264 NAL | 本地 dump 出合法 Annex-B NAL（ffprobe 可解析） | 是 |
| **S4** | 接 `esp_rtsp_service` Server（H264, 8554） | VLC / ffplay 能稳定拉流 | 是 |
| **S5** | 与屏幕预览 / LVGL 共存、分区定型、性能与稳定性收尾 | 预览与推流同时稳定 | 是 |

## 8. 风险与待确认清单

| # | 项 | 状态 |
| --- | --- | --- |
| T1 | C5 现有 AT 固件备份与回刷方式（拨码上=P4 / 下=C5，C5 有独立 UART） | ⚠️ 动手前确认 |
| T2 | `cp/` 与 host 的 SDIO bus width / packet-streaming / RPC version 对齐 | ⚠️ S1 实测 |
| T3 | 51 kΩ 上拉、3.3 V 电平、SDIO 时钟上限 | ⚠️ 查原理图 / 实测 |
| T4 | `C5 GPIO2 ↔ P4 GPIO6` 在 ESP-Hosted 中的功能定位 | ❓ 待确认 |
| T5 | `esp_wifi_remote` 是否与 `esp_hosted` 3.x 同时需要 | ❓ S1 实测 |
| T6 | `install.sh` 自带 IDF 版本与本工程 v6.1 是否冲突 | ❓ 待确认 |
| T7 | ISP 输出 RGB565 → YUV420 的具体改法（方案 A/B/C） | ❓ S3 定 |
| T8 | `esp_rtsp_service` 与 ADF 服务层在本工程的依赖兼容性 | ❓ S4 实测 |
| T9 | 分区重规划后 GUI 基线是否仍能编译 | ❓ S2 验证 |

## 9. 资料与出处

- `espressif/esp-hosted-mcu` README、`docs/getting-started-mcu.md`、`docs/architecture.md`、根 `idf_component.yml`（3.1.0 / `idf>=5.5`）
- `espressif/esp-hosted` 仓库 README（Linux 与 MCU 两条方案的分流）
- `espressif/esp-wifi-remote` README（推荐 esp-hosted 后端、`wifi_remote_over_at` 后端、50 Mbps 口径）
- ESP Component Registry：`espressif/esp_rtsp_service` v0.5.1、`espressif/esp_video` v2.3.0（m2m 示例）
- Espressif Developer Portal：《ESP H.264 Practical Usage Guide》（P4 硬件编码规格）
- `docs.espressif.com/projects/esp-vision/.../rtsp.html`（MicroPython `rtsp.RTSPServer`，仅作路径参考，本工程是 C/ESP-IDF）
- 本板资料：`ESP32-P4C5核心板OSPTEK.pdf` 第 5 页连接图、`ESP32-P4C5-Core核心板_引脚定义.pdf` 第 4 页、`鱼鹰P4C5-AT.md`
- 本工程：`doc/Matter-WebRTC摄像头方案适配评估.md`、`doc/摄像头裸显预览方案.md` 第 9 节、`partitions.csv`

## 10. 维护与沉淀条件

- 本文是**工程文档**。按用户约定「实际做出成绩之后再计入笔记」，**在 S4 真正跑通局域网 RTSP 之前，不沉淀到 `D:\Documents\Notes`**。
- 同步触发：C5 固件形态、SDIO 引脚结论、分区方案、组件依赖任一变化时，更新本文，并同步 `doc/工程介绍.md` 与 `AGENTS.md` 相关章节。

---

## 11. S1 实测记录（2026-11-11 晚，C5 网卡化 / 已完成软件侧）

用户确认 AT 固件可直接覆盖、拨码已切到 C5、同意方案后执行。**只动独立验证工程 `/home/he/esp_hosted`，本工程基线代码零改动。**

### 11.1 拨码验证

`/dev/ttyACM0` 用 `esptool chip_id` 读到的是 **ESP32-C5 rev v1.2**（Wi-Fi 6 双频、48 MHz 晶振、4 MB Flash），确认拨码已切到 C5 侧，而非 P4。

### 11.2 仓库与工具链

- `git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-hosted-mcu.git /home/he/esp_hosted`，版本 **3.1.0**。
- 子模块里带 `port/idf_components/esp_wifi_remote/esp_wifi_remote` → **关闭待确认项 T5**：3.x 内部以子模块形式集成 Wi-Fi remote 层，不需要再单独引入该组件；slave target 的 `SLAVE_IDF_TARGET` choice 就定义在其 `idf_v6.1/Kconfig.slave_select.in`。
- **没有跑 `install.sh` / `install.fish`**（那会另装一份 ESP-IDF），直接用本工程现有的 **IDF v6.1** + `idf.py` 构建 → **关闭待确认项 T6**：v6.1 完全可用。`export.fish` 只在需要用 `eh.py` 时才必须 source。
- **与第 4 节官方流程的差异**：官方用 `eh.py set-target / menuconfig / flash monitor`；本次用标准 `idf.py set-target / reconfigure / build / flash`，对 `cp/` 与 `mcu_host/` 这类标准 ESP-IDF 工程等价，且避免多装一套 IDF。

### 11.3 C5 侧 `cp/` 固件（已烧录）

| 项 | 结果 |
| --- | --- |
| 工程 | `examples/wifi/sta/cp/`，`idf.py set-target esp32c5` 一次通过 |
| **SDIO GPIO 默认值** | `CONFIG_EH_TRANSPORT_CP_SDIO_PIN_CMD=10 / CLK=9 / D0=8 / D1=7 / D2=14 / D3=13` —— **与本板 C5 侧逐脚一致，一个都不用改** |
| 传输 / 模式 | `CONFIG_EH_TRANSPORT_CP_SDIO=y`，`..._MODE_SW_AGGR=y`（rel-3 默认），`HIGH_SPEED=y` |
| 分区 | `partitions_eh_cp_ota_4m.csv`，匹配检测到的 4 MB Flash |
| 编译 | 1187 步零错误，`eh_cp_wifi_sta.bin` = `0x11a190`（约 1.1 MB），app 分区剩 37% |
| 烧录 | `idf.py -p /dev/ttyACM0 -b 460800 flash`，1,155,472 B，Hash 校验通过 |

C5 启动日志（验收判据已达成）：

```text
I (426) ehcp_core: ESP-Hosted core init
I (435) ehcp_core: - WLAN over SDIO
I (456) ehcp_core: ESP-Hosted Firmware version :: EH-3.1.0
I (463) ehcp_core: Transport used :: SDIO only
I (476) SDIO_SLAVE: Using SDIO interface (TX_MODE=0)
I (481) SDIO_SLAVE: SDIO datapath mode: SW_AGGR
I (638) ehcp_wifi: WiFi feature init ok
I (647) ehcp_core: ESP-Hosted core initialized
I (658) wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...
```

### 11.4 P4 侧 `mcu_host` 固件（已编译，待烧录）

| 项 | 结果 |
| --- | --- |
| 工程 | `examples/wifi/sta/mcu_host/`，`idf.py set-target esp32p4` |
| **必须改的一项** | 默认 `SLAVE_IDF_TARGET_ESP32C6=y`（官方对 P4 host 的默认）→ 改为 `CONFIG_SLAVE_IDF_TARGET_ESP32C5=y` 且 `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c5"`，`idf.py reconfigure` 后生效 |
| SDIO 引脚默认值 | `CMD=19 / CLK=18 / D0=14 / D1=15 / D2=16 / D3=17` —— **与本板 P4 侧逐脚一致**，无需改 |
| Reset | `CONFIG_ESP_HOSTED_HOST_RESET_GPIO=54` —— **与本板 GPIO54 一致**；`CP_RESET_STRATEGY_ALWAYS`，settle 1500 ms |
| 总线 | 4-bit，`CLK_KHZ=40000`（40 MHz），`RX_STREAMING_MODE=y` |
| 模式配套 | host 的 `RX_STREAMING_MODE` 是接收侧优化，与 CP 的 `SW_AGGR` 是 rel-3 默认配套组合，**不是**「Packet vs Streaming 不一致」→ **关闭待确认项 T2** |
| 编译 | 1199 步零错误，`wifi_sta_mcu_host.bin` = `0x8d620`，app 分区剩 45% |
| Wi-Fi 凭据 | 仍是示例占位 `myssid` / `mypassword`，**需用户提供真实 SSID/密码**才能验收 `got ip` |

> 官方 `host/Kconfig.ext` 里存在 `ESP_HOSTED_P4_C5_CORE_BOARD` 板型预设（P4+C5 核心板），进一步佐证本板形态在官方支持范围内；本次未启用该板型选项，因手工配置已逐项对齐。

### 11.5 尚未完成 / 需要用户配合

1. **烧录 P4 host 固件需要把拨码切回 P4（上 = P4）**；C5 固件已就位，切回不影响它。
2. 烧录后验收 `Received INIT event from ESP32 peripheral` / `Base transport is set-up`（**传输层握手，不需要 Wi-Fi 凭据**）。
3. `got ip` / ping / iperf 需要真实 AP 的 SSID 与密码，写入 host 工程 `sdkconfig` 后重编译。
4. 待确认项状态更新：T1（用户明确无需备份 AT 固件，已关闭）、T2 ✅、T5 ✅、T6 ✅、T4（`C5 GPIO2 ↔ P4 GPIO6` 未用到，握手成功即可视为不影响）、T3（51 kΩ 上拉/电平/时钟：默认 40 MHz 若握手失败再降到 5 MHz 验证）、T7/T8/T9 仍待后续阶段。
