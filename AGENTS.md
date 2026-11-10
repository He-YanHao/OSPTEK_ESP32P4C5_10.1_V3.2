# AGENTS.md — 本仓库的 AI 阅读与协作规则

> 适用对象：所有在本仓库内做阅读、分析、修改、生成的 AI / 自动化工具（WorkBuddy、Claude Code、Codex、Cursor、Copilot 等）。
> 使用其他工具时，请把本文件加入项目上下文（或建软链 / import 到 `CLAUDE.md`、`.cursorrules`、copilot instructions）。
> **本文件是约束，不是介绍**。项目概览看 `README.md`，完整技术细节看 `doc/工程介绍.md`。

## 0. 动手前的必读顺序

1. `README.md` — 项目定位、目录、当前状态
2. `AGENTS.md`（本文） — 边界与约束
3. `doc/工程介绍.md` — 架构、引脚速查、启动流程、已实现 / 未实现、已知坑
4. 目标模块的源码与其 `CMakeLists.txt`
5. 涉及硬件时，再查 `D:\Embedded\MCU\ESP32\ESP32-P4\鱼鹰光电` 下对应 PDF 原件

回答"架构 / 引脚 / 分区 / 版本"类问题前，必须先读第 3 步；**不要凭通用 ESP-IDF 经验推断本工程的具体数值**。

## 1. 不可变事实（不要"顺手纠正"）

| 事实 | 值 |
| --- | --- |
| 目标芯片 | `esp32p4`（RISC-V），`CONFIG_ESP32P4_REV_MIN_301=y` |
| 顶层工程名 | `p4`，`idf_build_set_property(MINIMAL_BUILD ON)` |
| 屏幕 | 10.1" JD9366，**物理 800 × 1280**，MIPI-DSI 2-lane，RGB565 |
| 逻辑方向 | 270° 旋转，GUI Guider 画布 1280 × 800 |
| 图形栈 | LVGL 9.4.0（本地组件 `lvgl__lvgl`）+ GUI Guider 2.0.1 + `esp_lvgl_adapter` 0.7.2 |
| 触摸 | JD9366，I2C0，SCL=GPIO8 / SDA=GPIO7；`CTP_RST=GPIO6`、`CTP_INT=GPIO21` 硬件有接线但驱动未用 |
| 存储 | 16 MB Flash / 32 MB HEX PSRAM @250 MHz；`factory` 分区仅 **768K**，`storage` FAT 15M |
| 版本约束 | `esp_lvgl_adapter 0.7.2` 声明 `idf >= 5.5.5`；`dependencies.lock` 固定托管组件版本 |

## 2. 文件边界（最重要）

| 路径 | 规则 |
| --- | --- |
| `guider/generated/**` | **禁止手改**。文件头写明 GUI Guider 自动生成、重新生成会覆盖。自定义逻辑放 `guider/custom/custom.c` 或新建组件 |
| `guider/guider.guiguider`、`guider/SCR.txt` | GUI Guider 工程 / 许可清单，由工具维护，改动需说明原因 |
| `guider/licenses/`、`guider/platform/` | 工具链资源，`.gitignore` 已排除，不要纳入提交 |
| `components/lvgl__lvgl/src/**` | LVGL 9.4.0 本地副本。**尽量不改**；确需改必须记录改动点，因为升级会被覆盖 |
| `managed_components/**` | IDF 组件管理器产物，**禁止修改**；要换版本改 `idf_component.yml` / `dependencies.lock` |
| `components/*/CMakeLists.txt` | 可改。`SRCS / INCLUDE_DIRS / REQUIRES` 必须显式、最小依赖；不要靠隐式传递 |
| `components/guider_ui/CMakeLists.txt` | 特例：用 `GLOB_RECURSE` 收集 `guider/generated` 与 `custom` 下 `.c`。新增源文件后必须 `idf.py reconfigure`（必要时 `fullclean`）让 CMake 重新扫描 |
| `main/main.c` | 只做装配（初始化 → 加锁 → `setup_ui`）。不要把业务逻辑堆进 `app_main` |
| `sdkconfig` | 用 `idf.py menuconfig` 修改，不要直接编辑；`sdkconfig.old` / `build/` 永不提交 |
| `partitions.csv` | 改动必须同时核算 `factory` 容量与 `storage` 偏移（现为 0x100000，总 16 MB） |
| `.workbuddy/**` | 项目数据，不是缓存，**不要删除** |
| `README.md` / `AGENTS.md` / `doc/**` | 可维护，但事实必须与代码一致（见第 7 节） |

## 3. 编码规范（与现有代码保持一致）

- 注释用**简体中文**，简洁，解释"为什么"而非复述代码；沿用现有 `@brief / @param / @return` Doxygen 风格。
- 错误处理统一 `esp_err_t` + `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE` / `ESP_GOTO_ON_ERROR(..., err, ...)`，不要裸 `if (ret != ESP_OK) return;`。
- 每个 `.c` 定义 `static const char *TAG = "<模块名>";`，日志用 `ESP_LOGI/E/W`。
- 硬件句柄 `static` 保存，对外只暴露 `xxx_get_panel()` / `xxx_get_handle()` 之类访问器。
- 头文件用 `#pragma once` 或 include guard，并带 `extern "C"` 包裹（C/C++ 混编）。
- 板级配置（GPIO、频率、lane 数、电压）集中为组件顶部宏，禁止在逻辑里散落魔法数字。
- 命名带组件前缀：`lcd_jd9366_*`、`touch_jd9366_*`、`gg_*`；新组件同理。
- 新建组件标准形态：`components/<name>/{CMakeLists.txt, <name>.c, <name>.h}`，并在 `main/CMakeLists.txt` 的 `REQUIRES` 中声明；不要让 `main` 直接 include 组件内部私有头。
- 不引入与 `dependencies.lock` 冲突的组件版本；不擅自 `idf.py update-dependencies`。

## 4. LVGL / UI 硬规则

1. **线程安全**：任何 LVGL 对象创建、属性修改、切屏，都必须在 `esp_lv_adapter_lock(-1)` / `esp_lv_adapter_unlock()` 之间（见 `main/main.c` 的 `setup_ui` 调用方式）。启动后的其他任务同理。
2. 锁内**禁止**阻塞 I/O、长延时、等待外部事件；数据先取好再加锁。
3. **旋转三处联动**：改 `lvgl_init.c` 的 `ROTATION_ANGLE_DEG`，必须同步核对
   - `H_RES / V_RES` 的条件编译映射，
   - `touch_jd9366.c` 的 `swap_xy / mirror_x / mirror_y`，
   - `guider/guider.guiguider` 的画布 `width / height`。
   只改一处必然出现"显示正常、触摸错位"。
4. `TEAR_AVOID_MODE = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI`；帧缓冲数量由 `esp_lv_adapter_get_required_frame_buffer_count()` 计算后传给 `lcd_jd9366_init(num_fbs)`，**不要写死**。
5. UI 改动优先在 GUI Guider 工程中修改并重新生成；临时热修放 `guider/custom/`，绝不落在 `guider/generated/`。
6. 颜色深度以 `components/lvgl__lvgl/lv_conf.h` 的 `LV_COLOR_DEPTH 16` 为准。`guider.guiguider` 里的 `colorDepth: "24"` 是 PC 模拟器设置，不要照搬到目标板。
7. 当前 UI 是键盘演示页（两个 `lv_keyboard`），仅用于验证链路，不要把它当成产品需求。

## 5. 硬件约束（改引脚前必查）

- 已占用：`LCD_RST=GPIO27`、`LCD_BL=GPIO20`（LEDC TIMER0/CH0，10 bit，20 kHz）、`TP_SCL=GPIO8`、`TP_SDA=GPIO7`、`CSI_RST=GPIO26`；MIPI DSI 用 `DSI_D0/1 ± / CLK ±`，CSI 用 `CSI_D0/1 ± / CLK ±`，均为**专用 PHY 引脚，不可任意重映射**。
- **`GPIO26` 是 `CSI_RST`（摄像头复位），`GPIO20` 才是背光使能**：底板 V1.3 为 `GPIO26 → R4 0 Ω → CSI_RST`（`R20` 10K 上拉到 1V8，图注要求"IO 使用开漏输出"），`GPIO20 → U2 SY7200 EN`（`R1` 10K 上拉到 +3V3）。两者无关，但历史上 `lcd_jd9366.c` 误把 `LCD_BL_GPIO` 写成 GPIO26，会和摄像头复位抢脚（IDF 报 `conflict found for GPIO[26]`）—— **已于 2026-11-09 修正为 GPIO20**。
- **摄像头 SCCB 与触摸共用 I2C0（GPIO7/GPIO8）**：`CSI_SDA_1V8 / CSI_SCL_1V8` 经 `U5 TXS0102DCUR`（VCCA=1V8、VCCB=+3V3、OE 常使能）转换后落到同一对引脚。GUI 模式下触摸先初始化；裸显模式不初始化触摸，可独占该总线。
- 摄像头 MCLK 由**板载 24 MHz 晶振**经 `R5` 0 Ω 提供（`J1.14`），P4 不需要输出 XCLK。
- `GPIO34~38` 是 P4 的 Strapping / 启动模式 / 下载脚（GPIO37/38 亦为 UART0 烧录脚）；复用前查核心板手册 3.3 节。
- `GPIO39~48` 是 `SD1_*`（SDIO / SD 卡相关，与 C5 协处理器通道相关），改动前查核心板引脚定义与底板原理图。
- `RTC_VBAT`（核心板 Pin 84）**不可悬空**（2.3–3.6 V）；MIPI D-PHY 由内部 LDO 通道 3 提供 **2.5 V**；底板 V1.3 的 CSI 复位上拉为 **1.8 V**，相关 IO 建议开漏输出。
- 显示异常先排查硬件：历史提交 `0b7ec1e` 记录过"崩溃是屏幕接口松动"，不要在未确认排线时反复改驱动参数。
- **摄像头链路硬规则（2026-11-10 实测，见 `doc/摄像头裸显预览方案.md` 第 8 节）**：
  - CSI 必须 `input=CAM_CTLR_COLOR_RAW8, output=CAM_CTLR_COLOR_RAW8`（`src==dst` 自动 bypass），`data_type` 全留 0；自定义 `bits_per_pixel` 会同时覆盖 `in_bpp/out_bpp`，使 `csi_transfer_size = h*v*in_bpp/64` 与传感器实际不符。
  - `lane_bit_rate_mbps` / `data_lane_num` 必须取自传感器格式的 `mipi_info`（SC2336 800x800 为 336 Mbps / 2 lane），不要照抄其它传感器示例的固定值。
  - 双缓冲必须每帧调 `esp_lcd_panel_draw_bitmap()` 提交；DPI 的 `cur_fb_index` 只在提交时更新，初始化后恒为 0。
  - **`frames` 计数只代表 DW_GDMA 定长传输完成，不代表有画面**；用日志里的 `uniq8`（真实图像 > 30、暗场常量约 3~5）或 `camera_preview_config_t.test_pattern` 判断链路与内容。
- **不得虚构引脚 / 电气参数**。手册里没查到就写"待确认"，并给出应查的 PDF；原理图页多为图片，文本提取可能不完整，结论以原件为准。

## 6. 构建与验证

```bash
idf.py set-target esp32p4     # 会重写 sdkconfig，先确认是否需要备份
idf.py build
idf.py -p <PORT> flash monitor
```

- 当前会话若没有 ESP-IDF 环境（`IDF_PATH` 未设置 / `idf.py` 不可用），**明确说明"未编译验证"**，不要声称构建通过，也不要伪造日志。
- 本机环境：ESP-IDF **v6.1**（`D:\Program\Espressif\v6.1\esp-idf`），激活脚本 `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`，串口 **COM7**。**Git Bash / MSys 不被 IDF 6.1 支持**，只能用 PowerShell / cmd。
- **构建可能随机卡死**（`riscv32-esp-elf-gcc` CPU 归零、日志停滞十分钟以上），与代码无关，是高并发 I/O 问题。`idf.py build` 不接受 `-j` 参数；遇到时杀掉 `ninja`/`gcc` 后，在 `build/` 下改用 **`ninja -j 4`** 继续，几十秒即可通过。
- 烧录可用 `idf.py -p COM7 flash`，但会先检测并可能触发重建；固件已是当前代码时，直接 `python -m esptool --chip esp32p4 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash @flash_args`（在 `build/` 下）更快。
- `sdkconfig` 中 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 与 `CONFIG_ESPTOOLPY_FLASHMODE="dio"` 不一致；涉及烧录 / Flash 配置时先用 `menuconfig` 核实，不要照抄任一项下结论。
- 提交前最小检查：CMake `REQUIRES` 是否闭合、是否误改生成物、分区余量（`idf.py size`）、旋转 / 触摸是否仍匹配、文档事实是否同步。
- 不要提交 `build/`、`sdkconfig.old`、`*.bin/*.elf/*.map` 等构建产物。

## 7. 需求边界与文档同步

- 用户已明确：**最终业务功能暂不公开**；但 **2026-11-09 已确认第一阶段验证目标**：先做一个**不链接 LVGL、不经过网络协议**的摄像头本地裸显预览（设计与分步验收见 `doc/摄像头裸显预览方案.md`）。因此：
  - 禁止臆测该功能是什么，禁止预造 API、文件名、组件行为；
  - `components/esp32at_sdio`（空 `CMakeLists.txt`）、音频、TF、USB、C5 无线仍描述为"**硬件已具备 / 组件占位，软件未接入**"；**MIPI-CSI 摄像头已接入**（`camera_preview`，链路已打通、正常成像待完成，见 `doc/摄像头裸显预览方案.md` 第 8 节），不要再写"软件未接入"；
  - 需求不清就提问，不要用"通常做法是……"代替本工程事实。
- **2026-11-09 实测进度**：`APP_MODE_CAMERA_RAW` 的 **S0（不链接 LVGL 的纯屏彩条裸显）已编译 / 烧录 / 实测通过**，日志与数据见 `doc/摄像头裸显预览方案.md` 第 5.1 节。因此：
  - **不要再声称"本机没有 ESP-IDF 环境 / 未编译验证"**。本机 IDF 为 **v6.1**（`D:\Program\Espressif\v6.1\esp-idf`），串口 **COM7**；PowerShell 下 dot-source `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` 激活。**Git Bash / MSys 不被 IDF 6.1 支持**（报 `MSys/Mingw is no longer supported`），编译烧录走 PowerShell / cmd。
  - `main/Kconfig.projbuild` 的 `APP_MODE` 默认 GUI，但当前 `sdkconfig` 已切到 `CAMERA_RAW`；切回需把两行改回（或 `git checkout -- sdkconfig`）并**重新 `idf.py reconfigure`**。
  - 改模式后**必须 `idf.py reconfigure` 再 build**；`MINIMAL_BUILD ON` 的条件 `REQUIRES` 有两阶段扫描陷阱，**不要删掉 `main/CMakeLists.txt` 里回读 `sdkconfig` 的兜底**（详见该文档 2.3 节）。
  - **2026-11-10 起主工作区为 Ubuntu 虚拟机**（`~/esp/OSPTEK_ESP32P4C5_10.1_V3.2/`，fish + `. ~/.espressif/v6.1/esp-idf/export.fish`，串口 `/dev/ttyACM0`）；代码只改虚拟机，由用户提交 git，其它机器从 GitHub 拉取，Windows 侧不再同步代码。
  - S1 / S2 / 第四轮链路定位均已完成；**唯一遗留是正常成像输出暗场常量、未出图**，禁止写成"摄像头预览已跑通 / 已出图"。
- 改动代码后，同步更新 `README.md`、`doc/工程介绍.md`、本文件中受影响的**引脚、版本、分区、状态、目录**。
- 引用硬件结论时标注来源（文件名 + 章节 / 页码），例如"核心板使用指南 3.3 节"。
- 面向用户一律用**简体中文**回复；命令、路径、代码标识符保持原文。
- `doc/Matter-WebRTC摄像头方案适配评估.md` 是**候选技术路径的评估稿**（Matter 1.5 / WebRTC / H.264 摄像头），**不构成已确定的业务需求**：
  - 不得据此在本仓库预造组件、API、文件名或初始化代码；
  - 文中 `P0 阻塞项` 与 `待确认清单` 均为未核实事项，回答相关问题时明确说"待核实"，不要当成工程现状；
  - 该文档**当前仅作为工程文档**，待实际跑通并产出可验证结果后再按第 9 节判断是否沉淀到笔记库。

## 8. 常见任务速查

| 要做的事 | 从哪里入手 |
| --- | --- |
| 改界面 / 加控件 | GUI Guider 工程重新生成 → `guider/generated/`；临时逻辑 → `guider/custom/custom.c` |
| 改屏幕方向 | `components/lvgl_init/lvgl_init.c` + `components/touch_jd9366/touch_jd9366.c` + `guider/guider.guiguider` |
| 调背光 / DSI 时序 | `components/lcd_jd9366/lcd_jd9366.c`、`esp_lcd_jd9366.h` |
| 加新功能模块 | 新建 `components/<name>/`，再在 `main/CMakeLists.txt` 声明 `REQUIRES` |
| 放图片 / 字体 / 大资源 | `storage`(FAT 15M) 或 `esp_lv_fs` / `esp_mmap_assets`，不要塞进 768K 的 `factory` |
| 接摄像头 / 无线 | 先查第 5 节引脚约束与 `doc/工程介绍.md` 资料索引；**需求未明确前不要实现** |
| 查硬件 | `D:\Embedded\MCU\ESP32\ESP32-P4\鱼鹰光电`，索引见 `README.md` 与 `doc/工程介绍.md` |

## 9. 外部笔记库与知识分流（2026-11-08 起）

个人技术笔记库：`D:\Documents\Notes`（**独立 git 仓库**，根有 `CLAUDE.md` / `README.md` 归档规范；改动只停留工作区，**不要 commit**）。

| 内容类型 | 落点 |
| --- | --- |
| 与本工程绑定的实现、板卡引脚占用、分区、组件状态、调试结论 | 本工程 `doc/工程介绍.md` / `README.md`（**默认落点**） |
| 鱼鹰 P4C5 的 AT 通道板卡速查 | `...\嵌入式\01_平台层（硬件）\ESP32\P4\鱼鹰P4C5-AT.md` |
| 与工程无关、与 ESP32-P4 平台强耦合的通用知识 / 可复用代码 | `...\01_平台层（硬件）\ESP32` 下已有子目录（优先 `P4/`） |
| 其它学科 | 按笔记库 `README.md` 的顶层边界规则归档 |

- 写 `鱼鹰P4C5-AT.md` **必须保持原有的极简风格**（标题 + 一张引脚表 + 少量要点），原有引脚表不得改写，禁止把工程介绍式长文搬进去。
- 笔记库里不放工程专属实现、不放未验证的驱动代码；P4 通用代码片段放 `ESP32/P4/ESP32-P4-通用代码骨架.md`。
- 两边事实必须一致；涉及引脚冲突（如 AT 表 P4 侧 GPIO14~19 与手册 `SD1_*` GPIO39~48 口径不同）时标注"待核对原图"，不要单方面改写。
- 笔记库 `CLAUDE.md` 要求：优先已有目录、跨主题只放一处、不使用 git 命令提交。
