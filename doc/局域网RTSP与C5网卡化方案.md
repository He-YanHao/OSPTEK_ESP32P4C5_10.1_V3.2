# 局域网 RTSP 与 C5 网卡化方案 · 本工程适配评估

> **状态：S1 / S2 / S3 / S4 已完整验收通过（2026-11-11 ~ 11-12）；局域网 RTSP 已跑通**。用户明确：Matter-WebRTC 暂缓，先做**局域网 RTSP**；并指出当前 C5 跑的 ESP-AT SDIO 固件不合适，希望「直接把 C5 当网卡使用」。
> **S2 实测**：本工程已接入 `esp_hosted`，`factory` 扩到 4M，新增 `APP_MODE_NET`，主工程内 `got ip:10.206.119.248` 且 ping 通（见第 12 节）。
> **S1 实测**：C5 已重刷 ESP-Hosted 3.1.0 `cp/` 固件，P4 跑官方 `mcu_host`，SDIO 握手 / RPC / Wi-Fi 控制面 / lwIP 数据面全部打通，`got ip:10.206.119.248`，同网段 Windows 主机 ping 通（0% 丢包）。详见第 11 节。摄像头裸显阶段（色块、闪烁已解决，偏绿发暗按用户要求不再追）见 `doc/摄像头裸显预览方案.md` 第 9 节。

## 0. 结论摘要

| 项 | 结论 |
| --- | --- |
| 推荐方案 | 把 C5 从 **ESP-AT SDIO 固件**换成 **ESP-Hosted 3.x 的 co-processor 固件（`cp/`）**，P4 侧得到标准 `lwIP / esp_netif / socket`，即真正的网卡 |
| 硬件是否需要改 | **不需要**。已按核心板手册原图核实：P4↔C5 就是 4-bit SDIO，且引脚与 ESP-Hosted 官方支持的 C5/P4 组合逐脚吻合 |
| IDF 兼容性 | ✅ `espressif/esp_hosted` **3.1.0** 要求 `idf >= 5.5`，targets 含 `esp32c5` 与 `esp32p4`；本工程 v6.1 在范围内 |
| RTSP 栈 | ✅ `esp_h264`（P4 硬件编码）+ **自写 `main/rtsp_server.{c,h}`**（RTSP 信令 + RTP/FU-A，零外部依赖）。官方 `esp_rtsp_service` 因依赖整套 ADF/GMF 生态、且示例锁 `esp_wifi_remote ~0.14.3`，评估后放弃（见 14.1） |
| 主要工作量 | ① 重刷 C5 固件并对齐 SDIO 配置（S1 ✅）；② 重规划 app 分区 768K→4M 并接入 esp_hosted（S2 ✅）；③ 不改采集链，直接编码 RGB565（S3 ✅）；④ 自写轻量 RTSP/RTP Server 并拉流验收（S4 ✅） |
| 建议 | **先在独立验证工程跑通 S1 再动本工程基线** —— ✅ S1（第 11 节）、S2（第 12 节）、S3（第 13 节）、S4（第 14 节）已全部完整验收通过；局域网 RTSP 可用 `ffplay -rtsp_transport tcp rtsp://<板子IP>:8554/live` 拉流 |

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

## 4. C5 固件更换步骤（S1，已在独立工程实测完成）

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
| `espressif/esp_rtsp_service` | v0.5.1, IDF release/v5.5+ | RTSP **Server** / Pusher / Puller。**S4 评估后未采用**：依赖 `esp_media_service` / `esp_service` / `gmf_core` / `media_lib_sal` / `esp_media_protocols` 整套 ADF 生态，示例锁 `esp_wifi_remote ~0.14.3` |

另有可参考的现成实践：ESP-ADF `examples/protocols/esp-rtsp`（旧 `esp_rtsp`），以及社区「ESP32-P4 + SC2336 720p + SDIO 接 C6 做 RTSP」案例（实测端到端延时约 110~220 ms），与本工程形态几乎一致。

### 5.3 最大改造点：ISP 输出格式

当前 `components/camera_preview/` 的 ISP 配置是 `output_data_color_type = ISP_COLOR_RGB565`，由 DMA 直写 DPI 帧缓冲；而 H.264 硬件编码器需要 **YUV420**。两者不兼容，必须二选一或加转换：

| 方案 | 做法 | 代价 |
| --- | --- | --- |
| A（最快验证 RTSP） | 暂时关掉本机屏幕预览，ISP 直接输出 YUV420 给编码器 | 屏幕上没有预览；但用户已明确「只要有正确的影像就行」，可接受 |
| B（预览 + 编码并存） | ISP 输出 YUV420，一路给 H.264，另一路经 PPA / 软件转 RGB565 写 DPI fb | 多一次格式转换与带宽占用，需评估 30fps 下的开销 |
| C（走官方标准链路） | 用 `esp_video` V4L2 重写采集，接 `/dev/video11` 编码器 | 最贴合官方示例，但现有 `camera_preview` 要重做 |

**S3 实测后本节结论已被推翻**：`esp_h264` 1.5.0 的 P4 HW encoder 原生支持 `ESP_H264_RAW_FMT_RGB565_LE`，因此**无需改 ISP 输出格式**，直接把 DPI 帧缓冲中部的 800×800 RGB565 喂给编码器，零拷贝、零转换，屏幕预览完整保留。A/B/C 三个方案均**未采用**，仅作历史记录；ISP 原生 YUV420 留作后续带宽优化备选。

### 5.4 与现有显示链的关系

用户已确认摄像头裸显「色块没有了、也不怎么闪烁了」，偏绿发暗**不再优化**。RTSP 阶段可以复用这条已验证的 CSI/ISP 采集链，只改输出格式与去向；`camera_preview` 里的双缓冲提交、WBG/gamma/color 等显示侧成果保留，不因换方案丢弃。

## 6. 分区与构建约束（必须处理）

- **分区（S2 已完成）**：`factory` 已由 768K 扩到 **4M**，`storage` 移到 `0x440000` / 12032K；16 MB 总量不变。原始 768K 时代 GUI 镜像占 98% 的瓶颈已解除；S3 加入 `esp_h264` 后镜像 `0xb13c0` 仍剩 83%。
- **MINIMAL_BUILD ON**：新增组件必须显式写进 `main/CMakeLists.txt` 的 `REQUIRES`；模式切换后仍需 `idf.py reconfigure`（既有坑，见 `AGENTS.md`）。
- **组件依赖**：S3 已验证 `esp_h264` 与现有 `camera_preview` / `esp_cam_sensor` / 网络链共存无冲突。S4 **未引入任何新组件**（`rtsp_server.c` 只用 lwIP socket），因此不存在 ADF 体系兼容性问题。
- `managed_components/**` 禁手改；改动经 `idf_component.yml` 走组件管理器。

## 7. 分步落地计划

| 阶段 | 内容 | 验收判据 | 是否动本工程基线 |
| --- | --- | --- | --- |
| **S0** | 核实硬件通道、组件版本、RTSP 栈（本文） | 一份可执行的引脚/版本清单 | 否（已完成） |
| **S1** | C5 刷 `cp/`，P4 跑官方 `mcu_host` Wi-Fi Station | `Received INIT event` + `Base transport is set-up` + `got ip` + ping/iperf | ✅ **已完成**：`got ip:10.206.119.248`、ping 0% 丢包；独立验证工程，本工程基线零改动 |
| **S2** | 本工程接入 `esp_hosted`，配 SDIO GPIO 18/19/14~17，重规划分区 | 主工程内 `got ip` + ping 通 | ✅ **已完成**：`got ip:10.206.119.248`、ping 3/3 通；分区 768K→4M，新增 `APP_MODE_NET`（第 12 节） |
| **S3** | 接 `esp_h264`，直接编码现有 RGB565 采集帧 → H.264 NAL | ffprobe / ffmpeg 解析通过 | ✅ **已完成**：`Constrained Baseline 800x800 yuv420p`、NAL `[SPS,PPS,IDR]`、解码 OK，30 fps `err=0`（第 13 节） |
| **S4** | 自写轻量 RTSP/RTP Server（H264, 8554） | ffprobe / ffmpeg 能稳定拉流 | ✅ **已完成**：四步握手 200 OK、ffprobe 识别 `h264 800x800`、ffmpeg 5 s 解出 149 帧 ≈30 fps（第 14 节） |
| **S5** | 与屏幕预览 / LVGL 共存、分区定型、性能与稳定性收尾 | 预览与推流同时稳定 | 是 |

## 8. 风险与待确认清单

| # | 项 | 状态 |
| --- | --- | --- |
| T1 | C5 现有 AT 固件备份与回刷方式 | ✅ 用户明确无需备份，已直接覆盖 |
| T2 | `cp/` 与 host 的 SDIO bus width / packet-streaming / RPC version 对齐 | ✅ 4-bit / SW_AGGR + streaming / RPC V2 全部协商成功 |
| T3 | 51 kΩ 上拉、3.3 V 电平、SDIO 时钟上限 | ✅ 40 MHz 一次握手成功，无需降速 |
| T4 | `C5 GPIO2 ↔ P4 GPIO6` 在 ESP-Hosted 中的功能定位 | ✅ 未用到，不影响链路 |
| T5 | `esp_wifi_remote` 是否与 `esp_hosted` 3.x 同时需要 | ✅ 仓库子模块自带，P4 host 侧自动引入 |
| T6 | `install.sh` 自带 IDF 版本与本工程 v6.1 是否冲突 | ✅ 跳过 install，v6.1 直接构建通过 |
| T7 | ISP 输出 RGB565 → YUV420 的具体改法（方案 A/B/C） | ✅ **无需改**：`esp_h264` 支持 RGB565_LE 输入，S3 直接编码现有帧 |
| T8 | `esp_rtsp_service` 与 ADF 服务层在本工程的依赖兼容性 | ✅ 评估后**不接入**，改为自写 `rtsp_server.c`，零新增依赖 |
| T9 | 分区重规划后其它模式是否仍能编译 | ✅ `APP_MODE_CAMERA_RAW` 在 4M 分区下编译通过（0x88c60，剩 87%），NET 亦通过 |

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

## 11. S1 实测记录（2026-11-11，C5 网卡化 / 已完整验收通过）

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

### 11.4 P4 侧 `mcu_host` 固件（已烧录并验收）

| 项 | 结果 |
| --- | --- |
| 工程 | `examples/wifi/sta/mcu_host/`，`idf.py set-target esp32p4` |
| **必须改的一项** | 默认 `SLAVE_IDF_TARGET_ESP32C6=y`（官方对 P4 host 的默认）→ 改为 `CONFIG_SLAVE_IDF_TARGET_ESP32C5=y` 且 `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c5"`，`idf.py reconfigure` 后生效 |
| SDIO 引脚默认值 | `CMD=19 / CLK=18 / D0=14 / D1=15 / D2=16 / D3=17` —— **与本板 P4 侧逐脚一致**，无需改 |
| Reset | `CONFIG_ESP_HOSTED_HOST_RESET_GPIO=54` —— **与本板 GPIO54 一致**；`CP_RESET_STRATEGY_ALWAYS`，settle 1500 ms |
| 总线 | 4-bit，`CLK_KHZ=40000`（40 MHz），`RX_STREAMING_MODE=y` |
| 模式配套 | host 的 `RX_STREAMING_MODE` 是接收侧优化，与 CP 的 `SW_AGGR` 是 rel-3 默认配套组合，**不是**「Packet vs Streaming 不一致」→ **关闭待确认项 T2** |
| 编译 | 1199 步零错误，`wifi_sta_mcu_host.bin` = `0x8d620`，app 分区剩 45% |
| 烧录 | `idf.py -p /dev/ttyACM0 -b 460800 flash`，Hash 校验通过 |
| Wi-Fi 凭据 | 已写入本机热点的真实 SSID / 密码（仅存在于虚拟机独立验证仓库的 `sdkconfig`，**未入 git、未入文档**） |

> 官方 `host/Kconfig.ext` 里存在 `ESP_HOSTED_P4_C5_CORE_BOARD` 板型预设（P4+C5 核心板），进一步佐证本板形态在官方支持范围内；本次未启用该板型选项，因手工配置已逐项对齐。

### 11.5 完整验收：INIT event → got ip → ping（全部通过）

把真实热点凭据写入 host 工程 `sdkconfig` 后重编译烧录（**凭据只存在于虚拟机独立验证仓库，未纳入本工程 git、未写入任何文档 / 笔记**），抓 30 秒串口，并从同网段主机做 ping 验证。

| 判据 | 实测结果 |
| --- | --- |
| 拨码切回 P4 | `esptool chip_id` → ESP32-P4 rev v3.2，确认切对 |
| P4 host 烧录 | `wifi_sta_mcu_host.bin` 0x8d620，Hash 校验通过 |
| SDIO 总线 | `SDIO 4-bit 40000 kHz CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54` |
| 复位与建卡 | `Reset co-processor using GPIO[54]` → `Card init success, TRANSPORT_RX_ACTIVE` → `SDIO Host operating in STREAMING MODE` |
| 协处理器识别 | `slave chip id: 0x17 (esp32c5)`，`capabilities 0x01`（WLAN over SDIO） |
| RPC / 版本 | `RPC version negotiated: V2`，`host=3.1.0 coprocessor=3.1.0 (match)` |
| 模式协商 | `SDIO SW_AGGR negotiated (e2h=15872B h2e=15872B)`，`slave=streaming host=streaming` |
| Wi-Fi 控制面 | `eh_wifi: set_config iface=STA ssid=「OnePlus」` → `esp_wifi_remote: esp_wifi_internal_reg_rxcb` |
| **lwIP 数据面** | `esp_netif_handlers: sta ip: 10.206.119.248, mask: 255.255.255.0, gw: 10.206.119.141` → `got ip:10.206.119.248` |
| **局域网双向通信** | Windows 主机 WLAN `10.206.119.76` → `ping 10.206.119.248`：**2/2 通、0% 丢包、152~160 ms、TTL=64** |

结论：**C5 已从 AT 协处理器变成真网卡**，P4 侧用标准 `lwIP / esp_netif / socket` 即可访问局域网，S1 验收判据全部达成，S2（接入本工程）的前提成立。

> 注：`Port 80` 不可达是预期的 —— host 示例没有起任何 TCP 服务，只验到 ICMP 回应；真实 socket 收发留到 S2 用 RTSP / iperf 验证。

### 11.6 S1 遗留与 S2 注意事项

- **凭据安全**：官方 `mcu_host/main.c` 连接成功时会把 SSID 与密码明文打印到串口，接入本工程时已删掉这行。**2026-11-11 深夜用户明确授权**（手机热点、外部无法物理接入、私密仓库），真实凭据自 S3 起随本地 `sdkconfig` 纳入 git；`sdkconfig.defaults.net` 仍用占位值。见第 13.4 节。
- **网络拓扑**：虚拟机 `ens33=192.168.74.128/24`（NAT 出网）、`ens37=192.168.126.128/24`（SSH 通道），都**不在热点网段** `10.206.119.0/24`；能直连板子的是 Windows 主机 WLAN 网卡 `10.206.119.76`。后续 RTSP 拉流应从 Windows 主机或同网段设备发起，或给虚拟机加到该网段的路由 / 改桥接。
- **P4 当前固件**：flash 里是 host 验证固件，**摄像头固件已被临时覆盖**；用主工程 `idf.py -p /dev/ttyACM0 flash` 即可烧回，代码与 git 不受影响。
- **C5 无需再动**：`cp/` 固件已烧好，拨码保持在 P4 侧即可，后续只改 P4 应用。
- **待确认项收口**：T1 ✅、T2 ✅、T3 ✅、T4 ✅、T5 ✅、T6 ✅、T9 ✅、T7 ✅（S3 已定：RGB565 直喂 HW encoder，无需 esp_video 重写）；T8 留 S4 验证。


## 12. S2 实测记录（2026-11-11 晚，本工程接入 ESP-Hosted / 已完整验收）

用户授权「开工」后执行，**只改虚拟机主工作区**；C5 保持 S1 烧好的 ESP-Hosted 3.1.0 `cp/` 固件，拨码保持 P4 侧，无需再动 C5。

### 12.1 分区表重规划

| 分区 | 原值 | 新值 |
| --- | --- | --- |
| `factory` | `0x40000`, **768K** | `0x40000`, **4M** (`0x400000`) |
| `storage` | `0x100000`, 15M | `0x440000`, **12032K** (`0xBC0000`) |

16 MB Flash 总量不变：`0x440000 + 0xBC0000 = 0x1000000`。GUI 基线此前镜像 `0xbbb60` 已占 768K 的 98%；4M 分区下 NET 模式镜像 `0x8ef10` 只占 14%（剩 86%）。

### 12.2 组件与模式

- `main/idf_component.yml`：`idf >= 5.5`，新增 `espressif/esp_hosted ^3.1.0` 与 `espressif/esp_wifi_remote >=1.3.1`。组件管理器实际解析为 **esp_hosted 3.1.0 / esp_wifi_remote 1.6.5**，并带入 `eppp_link` / `serial_slave_link` / `wifi_remote_over_eppp` 等传递依赖。
- **用组件注册表版本而非 `override_path`**：仓库从 GitHub 拉到任何机器都能自行下载，不依赖本机 `~/esp_hosted` 验证仓库。
- `main/Kconfig.projbuild`：新增 `APP_MODE_NET` 与 `APP_WIFI_SSID` / `APP_WIFI_PASSWORD`（默认占位，真实值只在本地 `sdkconfig`）。
- `main/CMakeLists.txt`：`MINIMAL_BUILD ON` 三分支，继续保留「回读 sdkconfig」兜底（两阶段组件发现）；NET 分支 `REQUIRES spi_flash esp_psram esp_hosted esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio`，**不链接 LVGL / camera_preview**。
- `main/main.c`：新增 `app_main_net()`；ESP-Hosted 由 `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` 在 `app_main` 之前自动完成 SDIO 握手，应用只跑标准 `esp_netif` + Wi-Fi STA。**刻意删掉了官方示例里明文打印 SSID / 密码的那行日志**。
- 新增 `sdkconfig.defaults.net` 作为配置模板（占位值）；真实凭据按用户授权随 `sdkconfig` 入库；`.gitignore` 排除组件管理器新下载的 `managed_components/*/`。

### 12.3 关键配置（与独立验证工程一致）

| 项 | 值 |
| --- | --- |
| slave target | `CONFIG_SLAVE_IDF_TARGET_ESP32C5=y`（官方 P4 host 默认 C6，**必须改**）、`CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c5"` |
| SDIO | 4-bit 40 MHz、`CMD19 / CLK18 / D0~D3=14/15/16/17`、`RESET=54`（全部为组件默认值，与本板一致） |
| 模式 | CP `SW_AGGR` + host `RX_STREAMING`，RPC V2 |
| FreeRTOS | `CONFIG_FREERTOS_HZ=1000`（主工程原本就是 1000，无需改） |

### 12.4 验收结果

`idf.py reconfigure` 下载组件一次通过（42 s）；`idf.py build` **1659 步零错误**，`p4.bin = 0x8ef10`，4M 分区剩 86%；烧录 Hash 通过。

串口（20 秒）：

```text
I (543)  eh_sdio: transport[host]: SDIO 4-bit 40000 kHz CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54
I (2247) eh_sdio: Card init success, TRANSPORT_RX_ACTIVE
I (2274) eh_init_evt: slave chip id: 0x17 (esp32c5)
I (2307) eh_init_evt: RPC version negotiated: V2
I (2311) eh_init_evt: esp-hosted fw versions: host=3.1.0 coprocessor=3.1.0 (match)
I (2579) eh_wifi: set_config iface=STA ssid=「OnePlus」 (7 bytes)
I (3194) main: wifi station started (credentials not logged)
I (3194) main: wifi sta start, connecting
I (4581) esp_netif_handlers: sta ip: 10.206.119.248, mask: 255.255.255.0, gw: 10.206.119.141
I (4581) main: got ip: 10.206.119.248, mask: 255.255.255.0, gw: 10.206.119.141
```

Windows 主机 `10.206.119.76` → `ping 10.206.119.248`：**3/3 通、0% 丢包、41~439 ms**（首包 ARP 耗时），`arp -a` 解析到 `44-bd-8d-d5-a1-f4`。

**T9 关闭**：切回 `APP_MODE_CAMERA_RAW` 在新分区表下编译通过（`p4.bin 0x88c60`，剩 87%），再切回 NET 编译通过（`0x8ef10`，剩 86%）；三模式互不影响。最终 `sdkconfig` 保持在 `APP_MODE_NET`。

### 12.5 本轮踩的坑

1. **漏了 `WIFI_EVENT_STA_START → esp_wifi_connect()`**：第一版只处理 disconnected，STA 启动后从不发起连接 —— SDIO 握手全过、Wi-Fi 配置也生效，但 25 秒都没有 `got ip`。补上后 1.4 s 内拿到 IP。这是 S2 唯一的功能性 bug。
2. **编辑工具产出的文件带 UTF-8 BOM**：`partitions.csv` 首行出现 BOM 字符，虽在注释行不影响解析，已统一清除；以后上传生成文件先查 BOM。
3. **凭据写入**：S2 阶段 `sdkconfig` 含真实密码且刻意不提交，S3 起用户已授权提交（见第 13.4 节）；写密码时工具回显会替换字符串，改用 base64 传参 + SHA1 校验。应用日志不打印 SSID / 密码。
4. **组件管理器副作用**：reconfigure 顺带把 tracked 的 `espressif__esp_lv_decoder` 从 0.5.0 更新到 **0.5.1**（修复 LVGL v9 文件头探测），涉及 CHANGELOG / `.component_hash` / `esp_lv_decoder.c`；属组件管理器产物，**已在 S4 收尾的全量提交中一并纳入**，新下载的组件目录仍由 `.gitignore` 排除。

### 12.6 下一步（S3）

- ✅ S3 已完成：改用 `esp_h264` 直接编码 RGB565，NAL 经 ffprobe / ffmpeg 验证合法（见第 13 节）。
- `socket()` 真实收发：**S4 已验收**（RTSP 四步握手 + ffmpeg 拉流解出 149 帧 ≈30 fps）；S2 当时只有 ICMP 回应。
- 当前 P4 flash 跑的是 NET 模式固件；切回摄像头只需把 `sdkconfig` 的 `APP_MODE` 改回并重编烧录。

## 13. S3 实测记录（2026-11-11 深夜，H.264 编码 / 已完整验收）

用户「继续往下面推进」，并明确授权把真实热点凭据纳入本私密仓库。

### 13.1 关键决策：不改采集链，直接编码 RGB565

调研推翻了第 5.3 节原本推荐的方案 A（ISP 改输出 YUV420）：

- `espressif/esp_h264` **v1.5.0** 的 P4 硬件编码器明确支持 `ESP_H264_RAW_FMT_RGB565_LE`（组件注册表 feature 表 HW encoder 列 = supported），且 IDR 帧自动带 SPS/PPS。
- IDF 6.1 同时确认 `CAM_CTLR_COLOR_YUV420` / `ISP_COLOR_YUV420` = `ESP_COLOR_FOURCC_OUYY_E_VYY`，即 ISP 原生也能输出编码器要的 YUV420；**两条路都通**，但 RGB565 路线对已跑通的采集链零改动。

因此采用「方案 D」：保留 `camera_preview` 的 CSI(RAW8 bypass) → ISP(RAW8→RGB565 + WBG/demosaic/gamma/color) → DMA 直写 DPI fb 全链路和屏幕预览，把 DPI 帧缓冲中部那块**连续 800×800 RGB565（1,280,000 B）直接作为编码器输入**，零拷贝、零格式转换。

### 13.2 代码结构

- 新增 `APP_MODE_H264`（第 4 个 `APP_MODE` 选项）：`main/CMakeLists.txt` 四分支，`REQUIRES` 同时含网络链（`esp_hosted esp_wifi esp_event esp_netif nvs_flash lwip`）、摄像头链（`camera_preview lcd_jd9366 esp_mm esp_lcd esp_driver_i2c esp_driver_gpio`）与 `espressif__esp_h264`；`MINIMAL_BUILD` 回读 sdkconfig 的兜底扩展为四模式。
- `main/idf_component.yml` 新增 `espressif/esp_h264 ^1.5.0`（实得 **1.5.0**）。
- `components/camera_preview` 新增可选帧回调 `camera_preview_set_frame_cb()`：在 `cam_rx_task` 每帧 `draw_bitmap` 提交后同步调用；默认 NULL，**摄像头基线行为完全不变**。
- `main/h264_sink.c`：`esp_h264_enc_hw_new(RGB565_LE, 800×800, GOP30, fps30, 2 Mbps, QP24~36)` → `esp_h264_enc_open()` → 每帧 `esp_h264_enc_process()`；输出缓冲用 `esp_h264_aligned_calloc(..., ESP_H264_MEM_SPIRAM)`。首个 IDR 帧既通过 **TCP:9999** 完整发给 PC，也通过串口 `NALHEX` 分块导出（32 B/行），后者不依赖网络。**（历史验收手段，S4 收尾已移除；现统一用 RTSP 拉流验收，见 14.6）**
- `main.c` 把网络初始化抽成 `app_net_init()` 供 NET / H264 共用；H264 分支顺序 `app_net_init() → lcd_jd9366_init(2) → h264_sink_start() → camera_preview_start()`。

### 13.3 验收：ffprobe / ffmpeg 解析通过

编译 1678 步零错误，`p4.bin 0xb13c0`（4M 分区剩 83%）。运行日志：

```text
I (3561) h264_sink: hw encoder ready: 800x800 RGB565_LE, out cap=1280000, GOP=30, fps=30
I (3561) h264_sink: NAL dump server listening on TCP:9999
I (3748) cam_prev: camera preview started
I (3809) h264_sink: first IDR NAL len=3046 type=0 head=00 00 00 01 67 42 c0 1f ... 00 00 00 01 68 ce 04
I (8755)  main: h264 frames=149 bytes=715695  idr=5  err=0 cam=149
I (13755) main: h264 frames=299 bytes=1464495 idr=10 err=0 cam=299
```

把 96 行 `NALHEX`（3046 B）还原成 `s3_idr.h264` 后，本机 ffprobe / ffmpeg：

```text
codec_name=h264  profile=Constrained Baseline  width=800  height=800  pix_fmt=yuv420p
Annex-B 起始码偏移 [0, 25, 34] → NAL 类型 [7 SPS, 8 PPS, 5 IDR]
ffmpeg 解码第一帧：DECODE_OK
```

**S3 判据达成**：产出可被标准工具解析的合法 Annex-B H.264 码流；30 fps 编码零错误，编码帧数与采集帧数 1:1，屏幕预览同步保留（`uniq8≈39`、`csi_err=0` 说明编的是真实画面）。

### 13.4 本轮踩的坑

1. **漏了 `lcd_jd9366_init(2)`**：`camera_preview` 要写 DPI 帧缓冲，H264 分支启动时报 `LCD panel not initialised / ESP_ERR_INVALID_STATE`；补上后正常。
2. **Wi-Fi 认证失败（S4 已澄清并解决，详见 14.4 第 2 条）**：本轮多次烧录后 STA 一直 `StaDisconnected reason=2 rssi=-34~-49`，拿不到 IP（S2 时曾成功 `got ip:10.206.119.248`）。已排除：密码变化（SHA1 `fcd7f581bd72` 与 S2 一致）、信号强度、认证模式（`netsh wlan` 显示热点为 WPA3-SAE `00-0f-ac:8`，已补 `WIFI_AUTH_WPA2_WPA3_PSK` + `WPA3_SAE_PWE_BOTH` 仍失败）。用户重启热点后经约 90~100 s 自行收敛，`got ip:10.206.119.248` 恢复且 ping 通。决定性对照：重烧 S1 曾成功的官方固件同样 `reason=2`，证明与 P4 代码无关。TCP:9999 / socket 数据面已由 S4 的 RTSP 拉流间接验证。
3. **串口导出导致短暂提交积压**：首帧 dump 约 0.8 s 阻塞 `cam_rx_task`，`commits` 比 `frames` 恒定落后 21 帧，但提交速率仍 30/s，不影响画面；仅首帧一次性行为。
4. **凭据策略变更**：用户明确「手机热点别人无法物理连接、且是私密仓库」，授权推送真实 Wi-Fi 密码。故 `sdkconfig` 自本阶段起纳入 git；`sdkconfig.defaults.net` 仍只保留占位值。若仓库日后转为公开或更换热点，应先清理 git 历史中的凭据。

### 13.5 下一步（S4，✅ 已完成）

S4 已完整验收，详见 **第 14 节**：自写 `main/rtsp_server.{c,h}`，`ffplay -rtsp_transport tcp rtsp://<板子IP>:8554/live` 可稳定拉流。ISP 原生 YUV420 仍作为后续带宽优化备选保留。

## 14. S4 实测记录（2026-11-12 凌晨，局域网 RTSP / 已完整验收）

### 14.1 关键决策：不用 `esp_rtsp_service`，自写轻量 RTSP/RTP Server

调研 `espressif/esp_rtsp_service` 0.5.1~1 后放弃接入：

- 依赖链是整套 esp-adf / GMF 生态：`esp_media_service`、`esp_service`、`gmf_core`、
  `media_lib_sal`、`esp_media_protocols`、`espressif/cjson`，还需
  `esp_media_service_link()` 把帧喂进服务总线。
- 官方示例锁 `espressif/esp_wifi_remote ~0.14.3`，与本工程 1.6.5 不是同一路，
  在 `MINIMAL_BUILD ON` 的精简工程里风险和编译体积都不划算。
- 本工程需求只是「单客户端 + H.264 + 局域网」，RTSP 信令与 RTP 打包合计约 500 行，
  用 lwIP socket 直接实现更简单、可排障。

落地为 `main/rtsp_server.{c,h}`，**零新增组件依赖**。

### 14.2 代码结构与数据路径

```
camera_preview (RGB565 800x800 @30fps, DMA 直写 DPI fb)
  └─ camera_preview_set_frame_cb()
       └─ h264_sink.c: esp_h264_enc_process() → Annex-B NAL
            └─ rtsp_server_push_frame()   [非阻塞，4 个 PSRAM slot 队列]
                 └─ rtsp_tx_task: 切起始码 → single-NAL / FU-A 分包
                      └─ RTP/UDP 或 RTSP TCP interleaved → 客户端
```

- RTSP 端口 **8554**，URL `rtsp://<板子IP>:8554/live`，支持
  `OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN / GET_PARAMETER`。
- `DESCRIBE` 从已缓存的 SPS(21 B) / PPS(5 B) 生成
  `sprop-parameter-sets`，并自动从 SPS 提取 `profile-level-id=42c01f`。
- `SETUP` 同时支持 `RTP/AVP/UDP;unicast;client_port=` 与
  `RTP/AVP/TCP;interleaved=0-1`。手机热点可能开 AP 隔离，**TCP interleaved 更可靠**。
- RTP：PT=96、90 kHz 时钟、每帧 +3000、SSRC 固定、FU-A MTU 1400。
- 独立 `rtsp_tx` 任务使网络拥塞不阻塞 `camera_preview` 的 DPI 帧提交。

`main/main.c` 的 `app_main_h264()` 在 `h264_sink_start()` 之后、
`camera_preview_start()` 之前调用 `rtsp_server_start(8554)`；心跳新增
`rtsp sess/pkts/bytes/drop` 计数。**屏幕预览链一行未改**，`APP_MODE_H264` 同时预览 + 推流。

### 14.3 验收结果（全部实测）

| 项 | 结果 |
| --- | --- |
| 编译 / 烧录 | `p4.bin 0xb48d0`，4M 分区剩 82%，零错误；烧录 Hash 通过 |
| 网络 | `got ip: 10.206.119.248 / gw 10.206.119.141`，Windows `10.206.119.76` ping 2/2 通 |
| RTSP 四步握手 | 手写客户端探针：`OPTIONS / DESCRIBE / SETUP / PLAY` 全部 `200 OK`，Session 正常 |
| ffprobe | `codec_name=h264 / Constrained Baseline / 800x800 / yuv420p / format_name=rtsp` |
| ffmpeg 解码 | 拉出一帧 PNG（82,445 B），画面为**真实摄像头影像**（绿色调、偏暗、有明暗层次），非测试图案 |
| 连续拉流 5 s | `frame=149`、约 **30 fps**、`speed=1.06x`、零解码错误 |
| 服务端统计 | `rtsp pkts=398 → 692`、`bytes=838470`、`drop=3`、`h264 err=0`、**零 panic / 零 error** |

拉流命令（Windows 同网段）：

```bash
ffprobe -rtsp_transport tcp rtsp://10.206.119.248:8554/live
ffplay  -rtsp_transport tcp rtsp://10.206.119.248:8554/live
```

### 14.4 本轮踩的坑

1. **`rtsp_srv` 任务栈溢出（已修）**：首版 `rtsp_session()` 叠了
   `rx[2048] + sdp[1024] + base[512] + extra[512]`，加上 `rtsp_reply()` 的
   `hdr[768]`，超过 5120 栈 → `Guru Meditation Error: Core 1 panic'ed
   (Stack protection fault)`，表现为客户端连接上却收不到 OPTIONS 响应、
   `ConnectionResetError 10054`。把大缓冲改为 `static`（单客户端串行无重入），
   任务栈 `5120→8192`、`rtsp_tx 4096→6144` 后消失。
   **排障手法**：被动串口监听 + 手写 RTSP 客户端逐步打请求，一次定位到服务器侧崩溃。
2. **手机热点 WPA3-SAE 认证收敛慢（已澄清）**：热点重启后 STA 持续
   `StaDisconnected reason=2 rssi=-46~-50`，约 **90~100 s** 才拿到 IP。
   决定性对照是**原封不动重烧 S1 那套曾成功的官方 `mcu_host` 固件**，同样 `reason=2`
   → 证明与 P4 应用代码、负载、认证参数、密码（SHA1 `fcd7f581bd72` 一致）无关，
   是热点侧状态。回退 `WPA2_PSK`、照抄官方 `PWE_BOTH + H2E` 均无区别；
   最终采用官方 S1 配置，等待热点自行收敛即可。
   **不要再为此改认证代码**；拉流前先 `ping` 确认板子在线。
3. **纯 NET / 纯 H264 模式对照排除负载假设**：切到无摄像头无编码的
   `APP_MODE_NET` 仍 `reason=2`，确认不是 ISP+H264+SDIO 并发把认证握手饿死。
4. **TCP interleaved 优于 UDP**：热点 AP 隔离可能挡住板子→PC 的 UDP 单播，
   ffprobe 用 UDP 失败（`-10054`），换 `-rtsp_transport tcp` 立即成功。
5. **S3 的 TCP:9999 / 串口 NALHEX 导出已于收尾时移除**（见 14.6）：dump server 常驻任务占栈与 512 KB PSRAM，串口分块导出会在首帧阻塞帧提交任务约 0.8 s；RTSP 拉流已是更直接的码流验收手段。

### 14.5 遗留与下一步（S5）

- 成像仍**偏绿发暗**，用户已明确「只要有正确的影像就行」，不追成像质量。
- 单客户端、无 RTCP、无鉴权；长时间运行与多客户端未测。
- `APP_MODE_H264` 已同时跑预览与推流，但 LVGL / GUI 链仍未与网络链共存验证。
- 若需省带宽：`CAM_CTLR_COLOR_YUV420` / `ISP_COLOR_YUV420` 已确认可用，
  可改为 ISP 原生 YUV420 输出，省去 RGB565 中间格式。
- 热点认证收敛慢属于外部 AP 行为；如需稳定开机即在线，可评估让 C5 开 SoftAP
  做独立局域网，完全绕开手机热点。

### 14.6 收尾整理与交付状态（2026-11-12）

用户确认「测试通过」后做了一次交付级整理，目标是让仓库与代码都进入可长期维护状态。

**代码整理（仅 `APP_MODE_H264` 分支，不改变功能）**

| 移除项 | 原因 |
| --- | --- |
| `h264_dump_server` 任务 / TCP:9999 监听 | S3 验收用的一次性通道；常驻任务占 4096 B 栈，且永远只服务一个客户端 |
| `s_dump_buf` 512 KB PSRAM 分配 | 同上，随 dump server 一起消失 |
| 首帧 96 行 `NALHEX` 串口分块导出 | 实测会让 `commits` 恒定落后约 21 帧（阻塞 `cam_rx_task` 约 0.8 s） |
| `main.c` 启动日志里的 `TCP:9999 dump` | 已无对应服务 |

保留：单行 `first IDR NAL len=... head=...` 日志（只打一次，确认首帧自带 SPS/PPS）、
`h264_sink_get_stats()` 四项计数、心跳里的 `h264= / rtsp sess/pkts/bytes/drop`。
`h264_sink.c` 由此收敛为纯粹的「RGB565 → HW 编码 → `rtsp_server_push_frame()`」组件。
整理后重新编译烧录并复验 RTSP 拉流，确认 30 fps 与零错误未受影响。

**全量提交**

此前几轮有意留在工作区不提交的 5 个文件一并纳入：lt6911 固件 hex（CRLF/LF 换行规范化）
与 `espressif__esp_lv_decoder` 0.5.0→0.5.1 的 4 个组件管理器产物。提交后
`git status` 干净。`.gitignore` 仍排除未被跟踪的 `managed_components/*/`，
`sdkconfig` 含真实热点凭据（用户已授权入库）。

**虚拟机工作区约定**

用户明确：`~/esp/` 是唯一允许放置本会话产物的位置，`~/` 顶层不要堆临时文件。
本轮把散落在 `/home/he` 顶层的补丁脚本、源码备份、串口监听日志、
独立验证仓库统一归档/迁移到 `~/esp/` 下（详见当日工作日志）。
后续所有临时脚本、备份与抓包日志一律写 `~/esp/` 内，不再污染家目录顶层。
