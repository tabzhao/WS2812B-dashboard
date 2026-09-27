# Dashboard Terminal · ESP8266 仪表盘终端

![Rust](https://img.shields.io/badge/Rust-2021%20edition-orange?logo=rust)
![C++](https://img.shields.io/badge/C%2B%2B-Arduino%20%2F%20PlatformIO-blue?logo=cplusplus)
![MCU](https://img.shields.io/badge/MCU-ESP8266%20(NodeMCU%20V3)-blue)
![Host](https://img.shields.io/badge/Host%20app-macOS%2013%2B-lightgrey?logo=apple)
![License](https://img.shields.io/badge/License-MIT-green)

一个**纯显示终端**：Mac 上的常驻上位机通过 UDP 单向推送「像素帧」和「表头值」，
NodeMCU V3 只负责把这两路数据渲染到硬件 —— 4 块 8×8 WS2812B 拼成的 **32×8 点阵**，
以及 **5 个动圈指针表头**。上位机失联超过 5 秒，点阵自动降级为 NTP 彩虹时钟，表头转演示扫针。

它没有业务逻辑，也不采集业务数据：上位机决定显示什么，终端只管画得稳、画得准。

```
     灯阵时钟模式（离线预览，tools/clock_preview.py 实际输出）

   01234567890123456789012345678901
0 |...#...##....##..####...##....#.|
1 |..##..#..#..#..#....#..#..#..##.|
2 |...#..#..#o....#...#.o....#...#.|
3 |...#...##.....##..#......#....#.|
4 |...#..#..#o....#.#...o..#.....#.|
5 |...#..#..#..#..#.#.....#......#.|
6 |..###..##....##..#.....####..###|
7 |============....................|
     '#' 数字  'o' 冒号  '=' 底行秒进度
```

## 特性

**固件（ESP8266）**
- NeoPixelBus **I2S DMA** 驱动 256 颗 WS2812B，CPU 全程空闲，不与 WiFi / PWM 抢中断
- 5 路 10-bit PWM 驱动动圈表头，带按实际 dt 缩放的缓动
- **手机配网**：上电自动开热点 + Captive Portal，WiFi 凭证存 EEPROM，**不用把 SSID 写死进固件**
- **mDNS 服务发现**：注册 `_dashboard._udp`，上位机自动找到设备，不需要知道 IP
- 上位机失联 5 s 自动降级：灯阵转 NTP 彩虹时钟，表头转演示扫针

**上位机（Rust + GPUI，macOS）**
- mDNS 自动发现，发现即选中第一台
- **灯阵与表头两路数据源独立设置**，可任意混搭：
  - 灯阵：`文字`（5×7 字模滚动）/ `频谱`（抓系统音频做 FFT）/ `无`（不推像素）
  - 表头：`系统指标`（CPU/内存/网速/磁盘 6 选 1）/ `音频频段`（低·中低·中·中高·高频）
- 音乐频谱用 **ScreenCaptureKit 直接抓系统音频**，零安装、不用装虚拟声卡
- 显式「开始推送 / 停止推送」开关；关窗口只是隐藏，菜单栏常驻，后台继续推
- 32×8 实时预览、两侧功耗封顶（上位机 + 固件各一次）

## 目录结构

```
.
├── firmware/            # PlatformIO 工程（ESP8266 / Arduino）
│   ├── src/             # 主固件
│   ├── include/         # config.h、配网、网络配置
│   │   └── secrets.example.h   # 凭证模板（复制为 secrets.h，已被 gitignore）
│   └── archive/stageN/  # 各阶段验证固件，排错回退用
├── host/                # 上位机（Rust + GPUI），仅 macOS
│   ├── src/spectrum.rs  # FFT 2048 → 线性+对数混合分 32 段 → 平滑 + 峰值白点
│   ├── src/audio.rs     # ScreenCaptureKit 抓系统音频
│   └── package_macos.sh # 打 Dashboard.app
├── tools/               # 三个纯标准库的 Python 验证工具（不需要任何依赖）
│   ├── clock_preview.py # 时钟 ASCII 预览 + 电流估算
│   ├── udp_sender.py    # 7 种图案推像素帧，验证映射与帧率
│   └── meter_test.py    # 6 种模式推表头值，验证 5 路 PWM 与缓动
└── docs/                # 设计文档（01 硬件 → 08 配网）
```

## 快速开始

### 1. 固件

```bash
cd firmware
# 复制凭证模板（可选：不配网也能用，上电会自己开热点让你填）
cp include/secrets.example.h include/secrets.h
pio run -t upload
```

首次上电 EEPROM 为空 → 自动开热点 `dash-XXXXXX`（后六位是 ChipId）→ 手机连上后
浏览器打开 `http://192.168.4.1` 填 SSID/密码 → 存 EEPROM，以后自动连。
长按 FLASH 键 5 s 强制重进配网。详见 [docs/08](docs/08-wifi-provisioning.md)。

> 烧录串口号要填进 `platformio.ini` 的 `upload_port`。
> **烧录不用拔灯阵**；真失败了先 `lsof /dev/tty.usbserial-*` —— 绝大多数情况是
> 串口被 Tabby / VSCode Serial Monitor 之类的第三方监视器占着。

### 2. 上位机

```bash
cd host
cargo run                 # 调试用
./package_macos.sh        # 打出 Dashboard.app，双击运行（推荐）
```

⚠️ 别直接双击 `target/release/dashboard_host` —— 它是裸的 Unix 可执行文件，
macOS 会拿 Terminal 当宿主来跑。

频谱模式需要一次授权：**系统设置 → 隐私与安全性 → 屏幕与系统音频录制**。
没反应就先跑 `cargo run -- --probe-audio` 自检。

## 硬件

| 部件 | 规格 |
|---|---|
| 主控 | NodeMCU V3（ESP-12E / ESP8266） |
| 点阵 | 4 × 8×8 WS2812B 模块串成一串，256 颗，逻辑 32×8 |
| 表头 | 5 × 动圈电流表，**内阻 1.2 kΩ、满偏 155.6 µA**（务必实测你自己的） |
| 表头限流 | 每路串 **20 kΩ / 1/4 W** 直连 GPIO → 155.7 µA ≈ 正好满偏 |
| 供电 | 单一 **5 V / 2 A** 插 microUSB，灯阵从 VU/VIN 取粗线 |
| 灯阵数据 | GPIO3 (RX) **直连 DIN**，3.3 V 直驱，无电平转换 |

⚠️ 两个会烧东西 / 让画面错乱的点，接线前务必读
[docs/01-hardware.md](docs/01-hardware.md)（供电与直驱）和
[docs/02-meter-driver.md](docs/02-meter-driver.md)（表头限流）：
**表头的 20 kΩ 一只都不能少**（省掉就是 17.7 倍过载，线圈瞬间烧毁）；
**全亮时务必实测灯阵端 VDD ≥ 4.75 V**，低于 4.5 V 会造成 ESP 欠压复位。

## 文档

| 文档 | 内容 | 何时读 |
|---|---|---|
| [docs/01-hardware.md](docs/01-hardware.md) | 硬件清单、拓扑、引脚、3.3 V 直驱可行性、供电与去耦 | **接线前** |
| [docs/02-meter-driver.md](docs/02-meter-driver.md) | 5 路动圈表头驱动与限流核算 | **焊表头前** |
| [docs/03-matrix-mapping.md](docs/03-matrix-mapping.md) | 32×8 像素索引映射（最容易错的一环） | **写任何渲染代码前** |
| [docs/04-udp-protocol.md](docs/04-udp-protocol.md) | 三类 UDP 包格式、状态机、超时规则 | 理解协议时 |
| [docs/05-power-budget.md](docs/05-power-budget.md) | 功耗预算、亮度上限、软件封顶算法 | 定亮度参数时 |
| [docs/06-clock-and-fallback.md](docs/06-clock-and-fallback.md) | 彩虹时钟排版、字模、色相循环 | 改时钟时 |
| [docs/07-host-integration.md](docs/07-host-integration.md) | **上位机对接接口（实现契约）** | **写上位机时以此为准** |
| [docs/08-wifi-provisioning.md](docs/08-wifi-provisioning.md) | 手机配网 Captive Portal | 首次烧录、换路由器时 |

## 设计定稿参数

| 项目 | 定值 |
|---|---|
| 逻辑分辨率 | 32 × 8（256 颗） |
| 像素索引公式 | `index = (x/8)*64 + (x%8)*8 + (7-y)`，**非蛇形、每块左旋 90°** |
| 灯阵驱动 | NeoPixelBus `NeoEsp8266Dma800KbpsMethod`（I2S DMA，**固定 GPIO3 = D9/RX**，硬件上换不了脚） |
| 灯阵帧率 | 60 fps（物理硬顶 129 fps） |
| 表头 | 5 路 PWM，协议侧 10 Hz，满偏 155.6 µA |
| UDP 端口 | 8551（由 mDNS SRV 通告，**不要写死 IP**） |
| 超时 | 灯阵 5 s → 彩虹时钟；表头 5 s → 演示扫针。两路独立计时 |
| 时钟格式 | HH:MM:SS（24h 含秒），**4×7 窄体字模**（5×7 需 37 列，放不进 32） |
| 全局功耗封顶 | 1000 mA（上位机侧 + 固件侧各算一次） |

## 三条不可妥协的约束

1. **必须用 NeoPixelBus 的 I2S DMA，禁用 Adafruit NeoPixel。** 后者靠关中断位翻转，
   256 颗一帧要连续关中断约 768 µs，会和 WiFi 协议栈、软件 PWM 抢中断 —— 表现为
   指针抖、UDP 丢包、偶发断连。代价只是数据脚被钉死在 GPIO3，且 `strip.Begin()` 必须晚于 `Serial.begin()`。
2. **`WiFi.setSleepMode(WIFI_NONE_SLEEP)` 必须开。** 默认的 modem sleep 会在 UDP 收包路径上
   插入 100~300 ms 延迟尖峰，不加这一行，60 fps 也会肉眼可见地卡。
3. **映射公式照抄，不要自行推导。** 按「逐行」或「蛇形」的直觉写，画面会彻底错乱，
   而且很难一眼看出是映射错而不是接线错。

## 开发进度

| 阶段 | 目标 | 状态 |
|---|---|---|
| 0 | 最小固件，只验证 32×8 映射 | ✅ 实测通过（`firmware/archive/stage0/`） |
| 1 | WiFi + UDP 收包 + 灯阵渲染 | ✅ 实测通过（`stage1/`） |
| 2 | 5 路表头 PWM + 缓动 + 扫针自检 | ✅ 实测通过（`stage2/`） |
| 3 | NTP + 彩虹时钟 + 超时降级 | ✅ 已实现，待长期真机观察 |

> 阶段 0 不能跳过：映射错了，后面所有调试都是在墙上找门。
> 顺序相对最初规划调过 —— **先表头、后时钟**，因为表头驱动要趁硬件在手时优先验证。

## 许可

MIT，见 [LICENSE](LICENSE)。
