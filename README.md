# OSPTEK ESP32-P4C5 · 10.1" V3.2

> 基于 **ESP32-P4**（+ ESP32-C5 无线协处理器）的 10.1 英寸 MIPI-DSI 显示 / 触摸 / LVGL 工程基线。
>
> **当前状态**：显示、触摸、LVGL 与 GUI Guider UI 骨架已跑通；`APP_MODE` 可切换 GUI 基线与 `CAMERA_RAW` 裸显。**2026-11-10 摄像头本地直显已完整跑通**：MIPI-CSI → ISP → DMA → DPI 全链路经测试图案（彩色条纹 → 灰色渐变）与用户目视验证，镜头朝向实物后正常成像 `uniq8=124~135`、`30 fps`、`csi_err=0`（此前暗场常量系摄像头反扣桌面、镜头被遮挡的物理原因，见 `doc/摄像头裸显预览方案.md` 第 8.5 节）。**虚拟机 `~/esp/OSPTEK_ESP32P4C5_10.1_V3.2/` 是唯一主工作区**，Windows 侧不再同步代码。`components/esp32at_sdio` 为空占位组件，音频 / TF / USB / 无线等硬件能力尚未在软件中接入。**2026-11-11 起下一步方向**：局域网 RTSP 推流，先把 C5 从 AT SDIO 固件换成 ESP-Hosted 网卡方案；评估与分步落地计划见 `doc/局域网RTSP与C5网卡化方案.md`（S1 C5 网卡化已于 2026-11-11 完整验收通过，见第 11 节），Matter-WebRTC 暂缓。**2026-11-11 晚 S1 完成**：C5 已重刷 ESP-Hosted 3.1.0 `cp/` 固件（AT SDIO 固件弃用），P4 经 4-bit SDIO 拿到 `10.206.119.248`，同网段 Windows 主机 ping 通、0% 丢包。**2026-11-11 晚 S2 完成**：本工程新增 `APP_MODE_NET` 并接入 `esp_hosted`，`factory` 由 768K 扩到 4M，主工程内 `got ip:10.206.119.248`、Windows ping 3/3 通。**S3 完成**：新增 `APP_MODE_H264`，P4 硬件编码 RGB565 帧，产出经 ffprobe / ffmpeg 验证合法的 Annex-B H.264 NAL（800x800 Constrained Baseline、30 fps、`err=0`）；详见 `doc/局域网RTSP与C5网卡化方案.md` 第 12 / 13 节。

## 一眼看懂

| 项目 | 规格 |
| --- | --- |
| 主控 | ESP32-P4，双核 HP RISC-V @400 MHz + LP 内核，芯片版本 ≥ rev 3.01 |
| 无线协处理器 | ESP32-C5HF4（Wi-Fi 6 2.4/5 GHz、BLE 5、802.15.4），板载 IPEX-1 天线座 |
| 硬件平台 | OSPTEK ESP32-P4C5-Module 开发板（底板 V1.3）+ ESP32-P4C5-Core 核心板（88 Pin 邮票孔） |
| 显示屏 | 10.1" YDP400BT001-V4，**800 × 1280**，驱动 IC **JD9366**，MIPI-DSI 2-lane |
| 触摸屏 | 与屏同接口的 JD9366 电容触摸，I2C，最多 5 点 |
| 图形栈 | LVGL **9.4.0**（本地组件）+ NXP GUI Guider **2.0.1** + `espressif/esp_lvgl_adapter` **0.7.2** |
| 显示方向 | 270° 旋转（横屏），GUI Guider 画布 **1280 × 800** |
| 存储 | 16 MB NOR Flash（80 MHz）+ 32 MB HEX PSRAM（250 MHz） |
| 分区 | nvs 16K / phy_init 4K / factory **4M** / storage(FAT) **12032K** @0x440000（2026-11-11 S2 扩容） |
| 框架 | ESP-IDF **v6.1**（本机 `D:\Program\Espressif\v6.1\esp-idf`），`CONFIG_IDF_TARGET="esp32p4"`；`esp_lvgl_adapter 0.7.2` 声明 `idf >= 5.5.5` |
| 工程名 | `p4`（见根 `CMakeLists.txt`） |
| 当前 UI | GUI Guider 生成的键盘演示页（两个 `lv_keyboard`），用于验证显示 / 触摸链路，非最终产品界面 |

## 快速开始

前置条件：已安装并激活 ESP-IDF 环境（`idf.py` 可用，串口驱动就绪），且 `managed_components/` 与 `dependencies.lock` 中的依赖已就位。

```bash
idf.py set-target esp32p4      # 首次配置目标芯片；会重写 sdkconfig，已有配置请先备份
idf.py build
idf.py -p <PORT> flash monitor # 例：-p COM7；115200 波特率
```

常用命令：

```bash
idf.py menuconfig              # 改配置请走 menuconfig，不要直接手改 sdkconfig
idf.py -p <PORT> monitor       # 仅看日志（也可走 USB-Serial-JTAG）
idf.py fullclean               # 依赖 / 组件变更后全量重编
idf.py size-components         # 关注 factory 分区余量（现为 4M）
```

### 切换运行模式

`main/Kconfig.projbuild` 提供 `APP_MODE`，默认仍是 GUI 基线：

| 模式 | 说明 | `main` 的 `REQUIRES` |
| --- | --- | --- |
| `APP_MODE_GUI`（默认） | LVGL + GUI Guider 基线 | `spi_flash esp_psram lvgl_init guider_ui` |
| `APP_MODE_CAMERA_RAW` | 不链接 GUI 链的裸显模式：`lcd_jd9366` + `camera_preview`，开机直接进入摄像头直显（DSI 彩条自检已移除） | `spi_flash esp_psram esp_mm esp_lcd esp_driver_i2c esp_driver_gpio lcd_jd9366 camera_preview` |
| `APP_MODE_NET` | **S2 网络基线**：C5 作 SDIO 网卡，标准 `lwIP / esp_netif / socket`，不链接 LVGL / 摄像头 | `spi_flash esp_psram esp_hosted esp_wifi esp_event esp_netif nvs_flash esp_driver_gpio` |
| `APP_MODE_H264` | **S3 编码**：网络 + 摄像头预览 + P4 硬件 H.264，NAL 经 TCP:9999 / 串口导出 | 上述网络链 + `camera_preview lcd_jd9366 esp_mm esp_lcd` + `espressif__esp_h264` |

```bash
idf.py menuconfig             # Application Mode → Camera raw preview
idf.py reconfigure            # 关键：MINIMAL_BUILD 下必须刷新 CMake 组件依赖
idf.py build
idf.py -p /dev/ttyACM0 flash    # Ubuntu 虚拟机；Windows 侧为 COM7
```

> 切回 GUI：选回 `APP_MODE_GUI` → `idf.py reconfigure` → build / flash。实测数据与两个构建坑见 `doc/摄像头裸显预览方案.md` 第 2.3 / 5.1 节。

> 配置提示：当前 `sdkconfig` 中 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 与 `CONFIG_ESPTOOLPY_FLASHMODE="dio"` 不一致，烧录前请用 `idf.py menuconfig` 确认实际 Flash 模式（详见 `doc/工程介绍.md` 第 15 节）。本板实测：烧录用 dio、bootloader 运行时切到 QIO，两种模式均可启动。

> **C5 侧固件（已更换）**：原厂预编译 AT 合并包 `ESP32C5_2.12.9_0X0.bin` 已于 2026-11-11 弃用并覆盖；当前烧的是 **ESP-Hosted 3.1.0 `cp/` 固件**（独立验证仓库 `~/esp_hosted/examples/wifi/sta/cp`），P4 侧由此获得标准 `lwIP / esp_netif / socket`。烧 C5 需把拨动开关往下拨、烧完拨回 P4；详见 `doc/局域网RTSP与C5网卡化方案.md` 第 11 节。**不要再烧 AT 合并包**，否则会破坏网卡化链路。

## 目录结构

```
CMakeLists.txt              顶层工程：project(p4)，MINIMAL_BUILD ON
main/                       app_main()：lvgl_init() → setup_ui(&guider_ui)
  main.c
  idf_component.yml         LVGL 指向本地 ../components/lvgl__lvgl
components/
  lcd_jd9366/               JD9366 MIPI-DSI 面板驱动、D-PHY 供电、背光 PWM
  touch_jd9366/             JD9366 电容触摸（I2C0）驱动
  lvgl_init/                显示 / 触摸 / esp_lv_adapter 的装配与启动
  guider_ui/                把 guider/ 下生成代码注册为 IDF 组件（GLOB_RECURSE 收集 .c）
  lvgl__lvgl/               LVGL 9.4.0 本地副本（便于改源码），含 lv_conf.h
  camera_preview/           摄像头直显：SC2336 → MIPI-CSI → ISP → DSI 帧缓冲（不依赖 LVGL）
  esp32at_sdio/             占位组件：当前只有一个空 CMakeLists.txt，未实现
guider/                     NXP GUI Guider 工程
  guider.guiguider          工程配置（版本、画布尺寸、屏幕列表）
  generated/                自动生成代码（screens / events / assets / utils）——勿手改
  custom/                   自定义逻辑入口 custom.c / custom.h / lv_conf_ext.h
  preview.png               UI 预览图
managed_components/         IDF 组件管理器拉取的第三方组件（勿手改）
partitions.csv              自定义分区表
sdkconfig / sdkconfig.old   当前 / 上一次配置
doc/工程介绍.md             完整工程介绍
AGENTS.md                   AI 协作 / 阅读规则
```

## 启动与显示链路

```
app_main()
  └─ lvgl_init()                       components/lvgl_init/lvgl_init.c
       ├─ esp_lv_adapter_get_required_frame_buffer_count()   计算防撕裂所需帧缓冲数
       ├─ lcd_jd9366_init(num_fbs)     LDO(2.5V) → DSI bus → DBI/DPI → JD9366 → DMA2D → 背光
       ├─ touch_jd9366_init()          I2C0 400 kHz → esp_lcd_touch
       ├─ esp_lv_adapter_init()        任务绑定 CPU1
       ├─ register_display/touch()     270° 旋转、防撕裂、输入设备注册
       └─ esp_lv_adapter_start()
  └─ esp_lv_adapter_lock(-1)
       └─ setup_ui(&guider_ui)         创建 layer_sys/top/bottom + screen，加载默认屏
```

LVGL 不是线程安全的：任何非 LVGL 任务内操作 UI，都必须用 `esp_lv_adapter_lock()` / `esp_lv_adapter_unlock()` 包住。

## 改配置前必读

| 约束 | 位置 / 值 | 说明 |
| --- | --- | --- |
| 屏幕旋转 | `ROTATION_ANGLE_DEG = 270`（`lvgl_init.c`） | 改它必须同步改触摸 `swap_xy / mirror_x / mirror_y` 和 GUI Guider 画布尺寸 |
| 面板物理分辨率 | 800 × 1280，RGB565，DPI 78 MHz / 60 Hz | `lcd_jd9366.h` / `esp_lcd_jd9366.h`，**不支持指令硬件转屏**，旋转靠适配层硬件（PPA / DMA2D） |
| DSI | 2-lane @1500 Mbps，LDO 通道 3 = 2.5 V | `lcd_jd9366.c` |
| 引脚 | LCD_RST=GPIO27，TP_SCL=GPIO8，TP_SDA=GPIO7，背光使能=GPIO20，CSI_RST=GPIO26 | `lcd_jd9366.c` 的 `LCD_BL_GPIO` 已按底板 V1.3 修正为 **GPIO20**（SY7200 EN）；GPIO26 是摄像头复位，勿复用 |
| 应用分区 | `factory` 4M（S2 由 768K 扩容） | 大字体、图片、资源仍优先放 `storage`(FAT 12032K) / mmap assets |
| Strapping | GPIO34~38（P4 启动模式 / 下载） | 复用这些脚做外设前先查核心板手册 3.3 节 |
| 电源 | `RTC_VBAT`（Pin84）不可悬空；CSI 复位上拉 1.8 V、IO 建议开漏 | 见底板 V1.3 与核心板手册 |

## 当前进度

| 模块 | 状态 |
| --- | --- |
| JD9366 MIPI-DSI 显示 + 背光 | 已实现并跑通 |
| JD9366 电容触摸（含旋转坐标） | 已实现并跑通 |
| LVGL 9.4.0 + esp_lvgl_adapter（270° / 防撕裂） | 已实现并跑通 |
| GUI Guider UI 骨架（键盘演示页） | 已生成，仅作链路验证 |
| 自定义分区表（4M app + 12032K FAT） | 已配置 |
| 裸显模式 `CAMERA_RAW` / S0 纯屏彩条（不链接 LVGL） | **2026-11-09 已实测通过**：236K 镜像、COM7 烧录、双缓冲彩条滚动 |
| S1 SCCB 探测（`cam_sccb_probe`） | **已实测通过**：GPIO7/8 扫到 `0x30 PID=0xcb3a`，复位自证 PASS |
| S2 摄像头直显（`camera_preview`） | **已实测**：800x800@30fps，CSI+ISP 零拷贝写入 DPI 帧缓冲，画面居中、上下各 240 行黑边 |
| S3 / 链路打通 + 正常成像（2026-11-10，Ubuntu 虚拟机） | **已实测通过**：测试图案彩条 → 灰度渐变验证全链路；镜头朝向白色小风扇后 `frames=30/s`、`commits` 1:1、`uniq8=124~135`、`csi_err=0`，真实图像数据有效。见 `doc/摄像头裸显预览方案.md` 第 8 节 |
| `components/esp32at_sdio` | **空占位**，历史上曾有 AT / SPI 桥接实现，现已删除，当前不参与构建 |
| 音频（ES8311 / NS4150 / 硅麦）、TF 卡、USB | 硬件已具备，软件未接入 |
| C5 无线（ESP-Hosted SDIO 网卡） | **S2 已跑通**：本工程 `APP_MODE_NET`，`got ip:10.206.119.248` + ping 3/3 通（S3 阶段热点侧认证待恢复） |
| P4 硬件 H.264 编码（`esp_h264` 1.5.0） | **S3 已跑通**：`APP_MODE_H264`，800x800 RGB565→NAL，ffprobe / ffmpeg 解析通过，30 fps 零错误 |
| 后续业务功能 | 需求待补充 |

## 硬件资料（本机路径）

资料目录：`D:\Embedded\MCU\ESP32\ESP32-P4\鱼鹰光电`

| 文件 | 用途 |
| --- | --- |
| `OSPTEK ESP32-P4C5-Module 开发板.pdf` | 开发板规格、已适配屏幕 / 摄像头清单、C5 固件烧录说明 |
| `ESP32-P4C5核心板OSPTEK.pdf` | 核心板产品说明与规格 |
| `ESP32-P4C5-Core核心板_使用指南2026.1.6.pdf` | 核心板硬件参数、88 Pin 定义、Strapping / 启动模式 |
| `ESP32-P4C5-Core核心板_引脚定义.pdf` | 88 Pin 速查表 |
| `ESP32-P4-Module_使用指南2026.09.10（含原理图）.pdf` | 模组功能、版本变更、原理图 |
| `ESP32P4模组基础底板V1.3.pdf` | 底板原理图（DSI / CSI / 触摸 / 电源网络名） |
| `ESP32-P4C5-Module开发板OSPTEK.pdf` | 开发板说明（另一版本导出） |
| `摄像头/ESP32-P4-摄像头.pdf` | 2MP 摄像头模组规格（SC2336，1920×1080@30fps） |
| `摄像头/SC2336_数据手册_V0.7.pdf` | 传感器寄存器、MIPI 1/2-lane、I2C 地址 7'h30 等 |

> 引脚 / 电气结论一律以原 PDF 为准，原理图类页面为图片，文本提取可能不完整。

## 文档导航

- `README.md`（本文）：快速上手与全局概览
- `AGENTS.md`：AI / 自动化工具阅读本仓库的规则与约束
- `doc/工程介绍.md`：完整工程介绍（架构、引脚、启动流程、扩展指南、已知坑）
- `doc/Matter-WebRTC摄像头方案适配评估.md`：Matter 1.5 / WebRTC / H.264 摄像头方案针对本工程的适配评估（**评估稿，非已确定需求**；当前仅作为工程文档）
- `doc/摄像头裸显预览方案.md`：第一阶段验证目标 —— 不链接 LVGL、不碰协议的摄像头本地直显（**S0~S2 实测记录 + 第 8 节链路打通 + 第 9 节开机稳定性与 ISP 色彩链**；本地直显已跑通，色块与闪烁已解决，偏绿发暗按用户要求暂不优化）
- `doc/局域网RTSP与C5网卡化方案.md`：**当前下一步方向** —— 局域网 RTSP 与 C5 网卡化（ESP-Hosted）评估稿，含板级 SDIO 引脚原图核实、组件版本与 S1~S5 落地计划（S1 / S2 / S3 已验收通过，见第 11 / 12 / 13 节；S4 RTSP Server 待实施）
- `guider/SCR.txt`：GUI Guider 生成内容的第三方许可清单
- 个人笔记库（工程外）：`D:\Documents\Notes\嵌入式\01_平台层（硬件）\ESP32\P4` —— `鱼鹰P4C5-AT.md`（AT 通道引脚速查）、`ESP32-P4-通用代码骨架.md`（平台通用代码）。分流规则见 `AGENTS.md` 第 9 节。

## 许可与来源

- 显示 vendor 驱动 `esp_lcd_jd9366.*` 来自 Espressif，Apache-2.0。
- LVGL 9.4.0 为 MIT；`components/lvgl__lvgl/` 是本地副本。
- `guider/generated/**` 由 NXP GUI Guider 2.0.1 生成，遵循其专有许可条款，见 `guider/SCR.txt`。
- `managed_components/**` 由 ESP-IDF 组件管理器管理，各组件自带 LICENSE / README。

---

硬件平台：OSPTEK 鱼鹰光电 <https://www.osptek.com/> · 仓库：`He-YanHao/OSPTEK_ESP32P4C5_10.1_V3.2`
