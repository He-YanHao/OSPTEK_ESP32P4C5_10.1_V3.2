# OSPTEK ESP32-P4C5 · 10.1" V3.2

> 基于 **ESP32-P4**（+ ESP32-C5 无线协处理器）的 10.1 英寸 MIPI-DSI 显示 / 触摸 / LVGL 工程基线。
>
> **当前状态**：显示、触摸、LVGL 与 GUI Guider UI 骨架已跑通；`components/esp32at_sdio` 为空占位组件，摄像头 / 音频 / TF / USB / 无线等硬件能力尚未在软件中接入。后续业务功能的需求待补充，本文档不对其做假设。

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
| 分区 | nvs 16K / phy_init 4K / factory **768K** / storage(FAT) **15M** |
| 框架 | ESP-IDF，`CONFIG_IDF_TARGET="esp32p4"`；`esp_lvgl_adapter 0.7.2` 声明 `idf >= 5.5.5` |
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
idf.py size-components         # 关注 factory 分区余量（仅 768K）
```

> 配置提示：当前 `sdkconfig` 中 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 与 `CONFIG_ESPTOOLPY_FLASHMODE="dio"` 不一致，烧录前请用 `idf.py menuconfig` 确认实际 Flash 模式（详见 `doc/工程介绍.md` 第 15 节）。

> **C5 侧固件（若后续要启用无线）**：开发板资料给出的预编译合并包为 `ESP32C5_2.12.9_0X0.bin`，烧录目标选 **ESP32-C5**（拨动开关往下拨）、地址 **0x0**，不要当应用烧到 0x10000；烧完拨回 P4。本仓库不含该固件，需从厂商资料仓库获取。

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
| 引脚 | LCD_RST=GPIO27，BL=GPIO26（LEDC 20 kHz），TP_SCL=GPIO8，TP_SDA=GPIO7 | 触摸无独立 RST / INT 引脚 |
| 应用分区 | `factory` 仅 768K | 字体、图片、资源不要硬塞进 app，考虑 `storage`(FAT 15M) / mmap assets |
| Strapping | GPIO34~38（P4 启动模式 / 下载） | 复用这些脚做外设前先查核心板手册 3.3 节 |
| 电源 | `RTC_VBAT`（Pin84）不可悬空；CSI 复位上拉 1.8 V、IO 建议开漏 | 见底板 V1.3 与核心板手册 |

## 当前进度

| 模块 | 状态 |
| --- | --- |
| JD9366 MIPI-DSI 显示 + 背光 | 已实现并跑通 |
| JD9366 电容触摸（含旋转坐标） | 已实现并跑通 |
| LVGL 9.4.0 + esp_lvgl_adapter（270° / 防撕裂） | 已实现并跑通 |
| GUI Guider UI 骨架（键盘演示页） | 已生成，仅作链路验证 |
| 自定义分区表（含 15M FAT） | 已配置 |
| `components/esp32at_sdio` | **空占位**，历史上曾有 AT / SPI 桥接实现，现已删除，当前不参与构建 |
| MIPI-CSI 摄像头（SC2336）、音频（ES8311 / NS4150 / 硅麦）、TF 卡、USB、C5 无线 | 硬件已具备，软件未接入 |
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
- `doc/摄像头裸显预览方案.md`：第一阶段验证目标 —— 不链接 LVGL、不碰协议的摄像头本地直显设计（**设计稿，未实现**）
- `guider/SCR.txt`：GUI Guider 生成内容的第三方许可清单
- 个人笔记库（工程外）：`D:\Documents\Notes\嵌入式\01_平台层（硬件）\ESP32\P4` —— `鱼鹰P4C5-AT.md`（AT 通道引脚速查）、`ESP32-P4-通用代码骨架.md`（平台通用代码）。分流规则见 `AGENTS.md` 第 9 节。

## 许可与来源

- 显示 vendor 驱动 `esp_lcd_jd9366.*` 来自 Espressif，Apache-2.0。
- LVGL 9.4.0 为 MIT；`components/lvgl__lvgl/` 是本地副本。
- `guider/generated/**` 由 NXP GUI Guider 2.0.1 生成，遵循其专有许可条款，见 `guider/SCR.txt`。
- `managed_components/**` 由 ESP-IDF 组件管理器管理，各组件自带 LICENSE / README。

---

硬件平台：OSPTEK 鱼鹰光电 <https://www.osptek.com/> · 仓库：`He-YanHao/OSPTEK_ESP32P4C5_10.1_V3.2`
