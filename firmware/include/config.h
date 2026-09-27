#pragma once

// =============================================================
//  阶段 3 · NTP 彩虹时钟 + 超时降级
//  映射已由阶段 0 实测确认，表头已由阶段 2 实测确认。
//  协议见 ../docs/04-udp-protocol.md
//  功耗与亮度见 ../docs/05-power-budget.md
//  表头驱动见 ../docs/02-meter-driver.md
//  时钟/彩虹/兜底见 ../docs/06-clock-and-fallback.md
// =============================================================

// ---- 屏幕几何（与参考工程一致）----
static constexpr uint8_t  kCols       = 32;
static constexpr uint8_t  kRows       = 8;
static constexpr uint16_t kPixelCount = kCols * kRows;   // 256

// NeoPixelBus 的 I2S DMA 方式固定使用 GPIO3 (RXD0)，构造时的 pin 参数会被忽略。
// 见 https://github.com/Makuna/NeoPixelBus/wiki/ESP8266-NeoMethods
static constexpr uint8_t  kLedPin     = 3;

// 板载 FLASH 按键（GPIO0 / D3）：短按切模式，长按切亮度。
// 注意：GPIO3 的 RX 被 DMA 占用，串口只能输出不能收命令，所以交互全靠这个键。
static constexpr uint8_t  kButtonPin  = 0;

// ---- 亮度档位（长按循环）----
// 全屏类图案（如第 7 档横向渐变）电流很大：
//   V=128 时 256 颗 ≈ 3.8A，会直接压垮 USB 供电。
//   默认停在 V=32（≈0.96A 理论值，实际灯珠通常低于此）。
// 若确认已换用外接 5V/2A 电源，可自行调高起始档位。
static constexpr uint8_t kBrightnessSteps[] = {16, 32, 64, 128};
static constexpr uint8_t kBrightnessCount  = sizeof(kBrightnessSteps);
static constexpr uint8_t kDefaultBrightnessIndex = 1;

// ---- 软件功耗封顶 ----
// 阈值 = 所有像素的 R+G+B 之和的上限。
// 换算： I(mA) = 0.0784 × 总和  →  12755 ≈ 1000 mA
// 详见 ../docs/05-power-budget.md
static constexpr uint32_t kMaxChannelSum = 12755;

// ---- 长按判定阈值 ----
static constexpr uint32_t kLongPressMs = 700;

static constexpr uint32_t kSerialBaud = 115200;

// ---- 开机黑屏窗口（默认关闭，保留备用）----
//
// 当初的假设：烧录时 ESP 跑 ROM bootloader，固件不执行，灯珠 latch 保持上次画面，
// 若上次是亮的则挂 1A 负载压垮电源 → 下载失败 → 所以每次都要拔灯阵。
//
// 2026-09-26 实测推翻了这个假设：灯阵照接 VIN、灯正常点亮、不按 RST、
// 460800 波特率直接 `pio run -t upload`，6.2 s 一次成功。
// 也就是说灯阵负载根本不影响下载，以前「必须拔灯阵」的真实原因是
// **串口被监视器占用**（Tabby 等第三方终端会独占 /dev/tty.*），
// 报错形如 "device reports readiness to read but returned no data"。
// 排查时先 `lsof /dev/tty.<设备>`，别去折腾硬件。
//
// 因此默认设 0：这个 3 秒延迟现在只是白拖慢开机。
// 保留常量是为了两种情况：
//   ① 改用更大电流/更弱的电源后，若开着上位机推全亮画面（约 1A）时烧录失败，
//      改回 3000 并按 RST 抢窗口即可；
//   ② 想观察启动时序时临时打开。
//
// 注意这只影响「点上传的那一刻灯是不是黑的」，跟 I2S 何时初始化无关：
// strip.Begin() 挪到 setup() 里任何一行都不影响下载阶段（那时固件还没跑）。
static constexpr uint32_t kBootBlackoutMs = 0;

// =============================================================
//  网络与协议（docs/04-udp-protocol.md）
// =============================================================

// 模块监听端口。上位机单播到此端口。
static constexpr uint16_t kUdpPort = 8551;

// ---- mDNS / DNS-SD 服务发现（docs/07-host-integration.md §发现）----
// 固件以 DHCP 拿 IP，并注册 _dashboard._udp 服务，上位机靠 browse 发现它。
// 零配置的好处：换网段、IP 变化都不用改代码。
//
// 主机名 = 前缀 + ChipId 后 6 位十六进制，天然唯一，多机不会撞名。
// 例：dash-a1b2c3.local
static constexpr const char* kMdnsServiceType = "dashboard";  // => _dashboard._udp
static constexpr const char* kMdnsHostPrefix  = "dash-";

// mDNS 总开关。1 = 注册 _dashboard._udp 让上位机自动发现；0 = 关闭，退回手动寻址。
//
// ★ 必须是宏而不是 constexpr：预处理器看不懂 C++ 变量。
//   `static constexpr uint8_t k = 1;` 写进 `#if k` 时，预处理器把它当「未定义的标识符」
//   → 求值为 0 → 整段代码被静默剔除，且不产生任何警告。这个坑踩过一次。
#define DASHBOARD_ENABLE_MDNS 1

// 固件版本。放进 mDNS 的 TXT 记录，上位机可以直接显示并据此排错。
static constexpr const char* kFirmwareVersion = "1.0.0";

// =============================================================
//  WiFi 配网门户（docs/08-wifi-provisioning.md）
// =============================================================
//  ★ 这是宏而不是 constexpr：预处理器看不懂 C++ 变量，
//    `#if kFlag` 里的 constexpr 会被当成未定义标识符求值为 0，
//    整段代码被静默剔除且零警告。这个坑踩过一次（代价是一次白烧）。
#define DASHBOARD_ENABLE_PORTAL 1

// 配网热点的密码。空字符串 = 开放热点（手机直连最省事，推荐）。
// 想加密码就填 8 位以上的 WPA2 密码，手机第一次连的时候要输它。
static constexpr const char* kPortalApPassword = "";

static constexpr uint16_t kPortalHttpPort = 80;   // 配置页 HTTP 端口
static constexpr uint16_t kPortalDnsPort  = 53;   // 通配 DNS（Captive Portal 靠它触发弹窗）
static constexpr uint8_t  kPortalMaxAps   = 20;   // 扫描结果最多缓存多少个

// 连 WiFi 的等待上限。超时就认为这份配置不可用 → 自动进配网门户。
// 定 15 s 而不是 20 s：配网失败时用户要干等，15 s 已足够区分
// 「密码错(几秒内失败)」和「信号差(一直转圈)」。
static constexpr uint32_t kWifiConnectTimeoutMs = 15000;

// 长按 FLASH 键多久强制进配网（换路由器 / 改密码时用，不用重新烧录）。
// 与切亮度的 700 ms 拉开一个数量级，避免误触。
static constexpr uint32_t kPortalHoldMs = 5000;

// 配网热点开了这么久还没人配置 → 自动重启重试一次。
// 防的是「路由器临时断电 → 设备开了一整天的热点没人管」。
static constexpr uint32_t kPortalTimeoutMs = 10UL * 60UL * 1000UL;

// 配网模式下灯阵的刷新间隔（10 fps）。
// 比正常时钟的 30 fps 低，是给 WebServer 和 DNS 让出 CPU ——
// 配网时画面只是个提示，流畅度没有意义。
static constexpr uint32_t kPortalFrameMs = 100;
// "SETUP" 呼吸周期与亮度范围
static constexpr uint32_t kPortalBreathMs = 2000;
static constexpr uint8_t  kPortalBreathMin = 18;
static constexpr uint8_t  kPortalBreathMax = 70;

// 包头 8 字节：magic(2) + ver(1) + type(1) + seq(2 LE) + brt(1) + rsv(1)
static constexpr uint8_t  kHeaderBytes       = 8;
static constexpr uint8_t  kMagic0            = 'M';
static constexpr uint8_t  kMagic1            = 'P';
static constexpr uint8_t  kProtoVersion      = 1;

static constexpr uint8_t  kTypeMeters        = 1;   // 5 × uint16
static constexpr uint8_t  kTypePixels        = 2;   // 768 B 像素帧
static constexpr uint8_t  kTypeClock         = 3;   // uint32 授时

// 像素帧：8 字节头 + 256 × 3 字节 = 776 B，远小于 1472，不触发 IP 分片
static constexpr uint16_t kPixelPayloadBytes = kPixelCount * 3;   // 768
static constexpr uint16_t kPixelsPacketLen   = kHeaderBytes + kPixelPayloadBytes;

// 收包缓冲区：给足余量，比合法最大包略大即可
static constexpr uint16_t kRxBufferBytes     = 1024;

// 固件侧亮度硬上限：即使上位机要求 255 也不超过此值。
// 这是电源的安全网，软件功耗封顶（kMaxChannelSum）是第二道。
static constexpr uint8_t  kMaxBrightness     = 128;

// 像素帧超时：超过此时间没收到 type=2 包 → 不再是 LIVE。
// 这是三态机的核心分界：有数据就显示上位机画面，没数据就降级。
static constexpr uint32_t kPixelTimeoutMs    = 5000;

// 诊断信息输出间隔
static constexpr uint32_t kStatsIntervalMs   = 1000;

// WiFi 断线后重试间隔
static constexpr uint32_t kWifiRetryMs       = 5000;

// =============================================================
//  NTP 时间源（docs/06 §6.7）
// =============================================================
// ⚠️ 必须用 POSIX 时区字符串 + configTzTime()，不要用 configTime(偏移)。
//    后者会让 time() 返回已偏移的值，再套 localtime() 会二次偏移。
//    "CST-8" = 东八区（POSIX 的符号与直觉相反，不要写成 "CST+8"）。
#define TZ_STRING  "CST-8"
#define NTP_SERVER_1  "ntp.aliyun.com"
#define NTP_SERVER_2  "ntp.ntsc.ac.cn"
#define NTP_SERVER_3  "pool.ntp.org"

// 时间有效性的判据。NTP 没同步上时 time() 返回 1970 附近的小值，
// 这个阈值（≈2023-11）能干净地把「已同步 / 未同步」分开，
// 从而决定降级到 CLOCK 还是 WAITING。
static constexpr uint32_t kValidUnixThreshold = 1700000000;

// SNTP 重同步间隔 2 小时。lwIP 默认是 60 分钟。
// 固件侧先试着调 lwIP（若符号可链接），无论如何都自带一个兜底定时器，
// 到点重调 configTzTime() 强制刷新 —— 两条路都开着，不怕某一路失效。
static constexpr uint32_t kNtpResyncMs = 2UL * 60UL * 60UL * 1000UL;

// type=3 授时包：与本地时间偏差超过此秒数才纠正，避免来回抖动
static constexpr uint32_t kTimeCorrectThresholdSec = 5;

// =============================================================
//  彩虹时钟布局（docs/06 §6.1~§6.5）
//  ★ 用户要求 HH:MM:SS（24 小时制，含秒）★
// =============================================================
//  32 列要塞下 6 位数字 + 2 个冒号，5×7 字模放不下（需 37 列），
//  所以改用 4×7 窄体字模：6×4 + 3 个组内间隔 + 2 个冒号区（各 2 列）= 31 列。
//
//   x:  0 │ 1-4 │5│ 6-9 │10│11│ 12-15 │16│ 17-20 │21│22│ 23-26 │27│ 28-31
//         │ H1  │ │ H2  │ :│  │  M1   │  │  M2   │ :│  │  S1   │  │  S2
//  y=0..6 │        HH : MM : SS        │
//  y=7    │ 秒进度条 32 px（0..59 秒） │

static constexpr uint8_t kClockWidth        = 32;
static constexpr uint8_t kClockTop          = 0;              // (8-7)/2
static constexpr uint8_t kDigitW            = 4;              // 窄体字模宽度
static constexpr uint8_t kDigitCount        = 6;              // HH MM SS
static constexpr uint8_t kDigitX[6]         = {1, 6, 12, 17, 23, 28};
static constexpr uint8_t kColonX[2]         = {10, 21};
static constexpr uint8_t kProgressBarRow    = 7;              // 第 7 行专属，与字模不冲突

// ---- 彩虹 ----
// 屏上第 x 列色相：hue = baseHue + kRainbowSpanDeg * x / 31
// 铺 300° 而非 360°，让左右两端有色差，看着像流动光带而不是一个闭环。
static constexpr uint16_t kRainbowSpanDeg   = 300;
static constexpr uint32_t kRainbowCycleMs   = 10000;          // 整体滚一圈

// ---- 时钟渲染 ----
// 时钟自己的帧率，与灯阵无关：30 fps 足够，再高只是白耗电。
static constexpr uint32_t kClockFrameMs     = 33;
// 明度档位（长按 LED 键循环）。彩虹 HSV 的平均通道和是 1.5×V，功耗吃紧，
// 所以起步就压低：参考工程实测观感对应的是 V=50，这里默认取 48。
//
// 改成 HH:MM:SS 后点亮颗数从 61 涨到 84（4×7 字模每位平均 12.6 颗）。
// 最坏组合是 00:00:00「数字最密 + 进度条全满」：
//   V=32 → 397mA | V=40 → 497mA | V=48 → 596mA | V=64 → 794mA
// 用 tools/clock_preview.py 可以自己复算。电源只有 1A 的话别用最高的那档。
static constexpr uint8_t  kClockBrightnessSteps[] = {32, 40, 48, 64};
static constexpr uint8_t  kClockBrightnessCount   = sizeof(kClockBrightnessSteps);
static constexpr uint8_t  kDefaultClockBrightnessIndex = 2;
// LIVE → CLOCK 的淡入时长（避免画面硬切）
static constexpr uint32_t kClockFadeInMs    = 250;

// ---- WAITING 兜底滚动 ----
static constexpr uint32_t kWaitingStepMs    = 80;             // 80 ms / 列
// WAITING 是异常态，用琥珀色单色而非彩虹：既传达"还没准备好"，也省电。
static constexpr uint8_t  kWaitingR = 180;
static constexpr uint8_t  kWaitingG = 100;
static constexpr uint8_t  kWaitingB = 20;

// ---- 底行（y=7）的秒进度样式 ----
// 时钟改成 HH:MM:SS 后 32 列被数字占满，原来的右侧 8×8 装饰区没有位置了，
// 秒的"动感"改由底行承担。短按 LED 键切换，现场对比后再把喜欢的设为默认。
enum SecRowVariant {
  SECROW_BAR   = 0,   // 秒进度条（默认）：0..59 秒映射到 32 px，走过部分由暗到亮
  SECROW_OFF   = 1,   // 关闭，底行全黑（最省电）
  SECROW_TICKS = 2,   // 刻度：每 5 秒一个亮点，当前 5 秒区间加亮
  SECROW_COUNT
};
static constexpr uint8_t kDefaultSecRowVariant = SECROW_BAR;

// =============================================================
//  按键（板载 FLASH / GPIO0）
// =============================================================
// 串口 RX 被 I2S DMA 占用，收不到命令，交互全靠这一颗键。
static constexpr uint32_t kButtonDebounceMs = 40;

// =============================================================
//  动圈表头（docs/02-meter-driver.md）
// =============================================================

// =============================================================
//  电参数 —— ★ 硬件实测为准，不要只看色环 ★
//
//  这三个值决定 duty=100% 时指针停在行程的百分之几，固件会在启动时
//  按它们算出实际电流并自检（过载/欠行程都会打印警告）。改完记得核对
//  启动日志里的那几行。
//
//  反推法（不用万用表也能定）：上位机 hold 100，看指针停在行程百分之几 P，
//  则 R_series = 3300000 / (P% × 20) − 1200 Ω
// =============================================================
// =============================================================
//  电参数 —— 2026-09-18 实测（可调电源 3.3 V，回路串 20 kΩ）
//
//  实测：3.3 V 下电流 0.14 mA，指针走到 90%
//    → 满偏电流 Ifs = 0.14 / 0.90 = 0.1556 mA = 155.6 µA
//    → 若要 100% 占空比正好满偏：R_total = 3.3 / 155.6µA = 21.2 kΩ
//                                R_series = 21.2k − 1.2k = 20 kΩ ✅
//
//  ⚠️ 早期记录的「20 µA 满偏」是错的，差了 7.8 倍，据此算出的
//     163.8 kΩ / 200 kΩ 全部作废。以本节为准。
//
//  实测电流 0.14 mA 略低于 20 kΩ 的理论值 0.156 mA，最可能的原因是
//  万用表电流档的内阻分摊了约 2.4 kΩ，或实际所焊电阻为 22 kΩ 标准值
//  （22k + 1.2k = 23.2 kΩ → 142 µA，与 0.14 mA 完全吻合）。
//  两种情况都不影响结论：真接 GPIO 时大约就是 90~100% 行程。
// =============================================================
static constexpr float kMeterSeriesOhm   = 20000.0f;  // 每路串在正极的电阻（1/4 W 绰绰有余）
static constexpr float kMeterInternalOhm = 1200.0f;   // 表头内阻
static constexpr float kMeterFullScaleUa = 155.6f;    // 实测推算的满偏电流

// 是否按上面三个数自动限幅（把 duty 压到「刚好满偏」，防撞止档）。
// 【默认关闭】—— 在参数还没测准之前开启，只会让指针走得更少，掩盖真实问题。
// 确认阻值与额定电流、且确认确实过载之后，再改成 true。
static constexpr bool kMeterAutoClamp   = false;

static constexpr uint8_t kMeterCount = 5;

// GPIO 5/4/14/12/13 = D1/D2/D5/D6/D7
// 这五个是 NodeMCU 上仅剩的支持 PWM、且不受启动电平约束的引脚。
// ⚠️ 千万不要用 GPIO16 (D0)：它是 RTC GPIO，analogWrite 对它不生效。
//    也不要用 GPIO0/2/15：strapping 引脚，上电瞬间会输出杂散脉冲。
static constexpr uint8_t kMeterPins[kMeterCount] = {5, 4, 14, 12, 13};

// PWM：core 3.x 的默认分辨率只有 8 bit(255)，必须显式设成 1023 才是 10 bit。
// 频率 1 kHz；若听到线圈啸叫，改成 20000（分辨率不受影响）。
static constexpr uint16_t kPwmRange  = 1023;
static constexpr uint32_t kPwmFreqHz = 1000;

// 表头渲染节拍 100 Hz。动圈表头机械带宽只有 2~5 Hz，
// 高于此毫无意义，纯粹是为了给缓动提供平滑的时间轴。
static constexpr uint32_t kMeterTickMs = 10;

// 指数缓动系数：displayed += (target - displayed) * alpha
// @100Hz 时 τ ≈ 200 ms。上位机 10 Hz 发包，本就有 100 ms 台阶，
// 这层缓动就是把台阶磨掉 —— 它是「渲染层的惯性」，不是业务逻辑。
static constexpr float kMeterAlpha = 0.05f;

// 表头超时：超过此时间没收到 type=1 包 → target 归 0，指数缓落。
// 与灯阵超时独立计时（可出现「画面在走、表头已归零」的组合态）。
static constexpr uint32_t kMeterTimeoutMs = 5000;

static constexpr uint16_t kMeterPayloadBytes = kMeterCount * 2;             // 10
static constexpr uint16_t kMetersPacketLen   = kHeaderBytes + kMeterPayloadBytes;  // 18

// type=3 授时包：8 字节头 + uint32（LE）= 12 B
static constexpr uint16_t kClockPacketLen    = kHeaderBytes + 4;            // 12
// 每路的软件微调系数（%% 级表头差异 / 想要更大摆幅时在这里整）。
// trim > 1.0 会在 firmware 侧被截到 1.0，不会过载。
static constexpr float kMeterTrim[kMeterCount] = {1.00f, 1.00f, 1.00f, 1.00f, 1.00f};

// ---- 上电扫针自检：0 → 满偏 → 0 ----
// 一次跑完 5 路，用来确认 5 个 PWM 通道硬件上都通、指针没卡死。
static constexpr uint32_t kSweepRampMs = 900;
static constexpr uint32_t kSweepHoldMs = 400;
static constexpr uint32_t kSweepDownMs = 900;

// ---- 演示图案：从未收到过表头包时自动跑 ----
// 5 路错相三角波。选三角波而非正弦，是因为斜率恒定，
// 最容易看出「指针在某一段卡住」或「低值区不动」。
static constexpr uint32_t kDemoPeriodMs = 8000;

// 表头诊断输出间隔（比灯阵那行更频繁，便于边调边看）
static constexpr uint32_t kMeterStatsMs = 500;
