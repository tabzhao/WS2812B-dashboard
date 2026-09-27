// =============================================================
//  阶段 3 · NTP 彩虹时钟 + 超时降级
//
//  阶段 2 之上叠加：
//    · NTP 自同步（CST-8，2 小时重同步）
//    · 三态机 LIVE / CLOCK / WAITING
//    · HH:MM:SS 彩虹时钟（4×7 窄体字模）+ 底行秒进度（3 种样式可现场切换）
//    · type=3 授时包接收
//
//  状态机：收到灯阵 UDP 包 → LIVE 显示上位机画面；
//          5 秒没有灯阵 UDP 包 → 切回 CLOCK 显示时钟。
//
//  映射公式已由阶段 0 实测确认，见 docs/03-matrix-mapping.md
//  表头驱动见 docs/02-meter-driver.md（§2.11 为实际接法核算）
//  协议见 docs/04-udp-protocol.md，功耗见 docs/05-power-budget.md
//  时钟/彩虹/兜底见 docs/06-clock-and-fallback.md
// =============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NeoPixelBus.h>
#include <time.h>
#include <sys/time.h>

// ★ config.h 必须在下面那个 #if 之前引入，否则预处理器此时还不认识开关宏，
//   会把 `#if DASHBOARD_ENABLE_MDNS` 判成 0 —— 整段 mDNS 代码会被静默剔除。
#include "config.h"

#if DASHBOARD_ENABLE_MDNS
  // mDNS 让上位机不必知道 IP 就能发现本设备（docs/07-host-integration.md）
  #include <ESP8266mDNS.h>
#endif

// WiFi 配置不再写死在代码里：存在 EEPROM，由手机配网页面填写（docs/08）
#include "netconfig.h"
#if DASHBOARD_ENABLE_PORTAL
  #include "portal.h"
#endif

// 优先使用本地 secrets.h（已在 .gitignore 中），没有则回退到模板
#if defined(__has_include)
  #if __has_include("secrets.h")
    #include "secrets.h"
    #define DASHBOARD_HAVE_SECRETS 1
  #endif
#endif
#ifndef DASHBOARD_HAVE_SECRETS
  #include "secrets.example.h"
#endif

// SNTP 重同步间隔本想调 lwIP 的 sntp_set_update_delay()，但在本 core
// (framework-arduinoespressif8266 3.1.2 / lwip2) 里实测不可链接 ——
// tools/sdk/lwip2/include/lwip/apps/sntp.h 根本没有这个声明，
// SDK 自带的 tools/sdk/include/sntp.h 也只导出 get/set_timezone 那几个。
// 所以走固件侧方案：到点重调 configTzTime() 强制刷新（见 startNtp / loop）。
// 这条路不依赖任何 lwIP 内部符号，永远不会因为 core 升级而失效。

// -------------------------------------------------------------
//  灯总线
//  I2S DMA 固定占用 GPIO3 (RXD0)，构造时的 pin 参数会被忽略。
// -------------------------------------------------------------
NeoPixelBus<NeoGrbFeature, NeoEsp8266Dma800KbpsMethod> strip(kPixelCount);

static uint8_t gFrameXY[kPixelCount * 3];
static uint8_t gRxBuf[kRxBufferBytes];

static WiFiUDP gUdp;

// -------------------------------------------------------------
//  三态机
// -------------------------------------------------------------
enum DisplayState {
  ST_WAITING = 0,   // 没像素数据 + 时间未同步
  ST_CLOCK   = 1,   // 没像素数据 + 时间已同步
  ST_LIVE    = 2,   // 有像素数据（上位机最高优先级）
  ST_CONFIG  = 3    // 配网模式：开着热点等手机来配（优先于上面三者）
};

static DisplayState gState         = ST_WAITING;
static uint32_t     gStateEnteredMs = 0;
static uint32_t     gLastPixelPacketMs = 0;

// -------------------------------------------------------------
//  超时判定（★ 长期运行的正确性就靠这一个函数）
// -------------------------------------------------------------
//  直接写 (int32_t)(now - last) < timeout 有三个坑：
//    ① 差值超过 2^31 ms（≈24.8 天）时转成有符号会变负，反而判定成"刚刚收到包"；
//    ② last 一直不更新时差值无限变大，millis() 归零（49.7 天）后会绕回一个很小的值。
//    ③ ★ lastMs 必须 ≤ now，否则无符号减法下溢成接近 2^32 的巨大值，立刻误判超时。
//       这条最阴 —— 它只在「收包时的 millis() 比 loop 开头的 now 晚」时发生，
//       表现为画面偶发闪一下，且不随着什么规律出现。见下面 handlePacket 的时间戳约定。
//  这里改成纯无符号比较，并在超时后把 last 钳到「刚好 timeout 之外」——
//  时间戳不再无限变老，上述 ①② 两种情况都不会误判。
static bool withinTimeout(uint32_t& lastMs, uint32_t now, uint32_t timeoutMs) {
  // 兜底 ③：lastMs 落在 now 之后，说明调用方混用了两个时刻的时间戳。
  // 语义上等价于「刚刚收到」，按未超时处理，而不是让它下溢成巨大值。
  if ((int32_t)(now - lastMs) < 0) return true;
  if (now - lastMs < timeoutMs) return true;
  lastMs = now - timeoutMs;
  return false;
}

// -------------------------------------------------------------
//  表头通道状态
// -------------------------------------------------------------
struct MeterCh {
  uint8_t  pin;
  float    trim;      // 软件微调
  float    target;    // 0..kPwmRange
  float    display;   // 0..kPwmRange（缓动后的实际显示值）
  uint16_t lastDuty;  // 已写入硬件的占空比，用于避免重复写
};
static MeterCh gMeters[kMeterCount];

// 占空比的安全上限。默认满量程，若自检发现串联电阻偏小（会超额定电流），
// 就被压到「刚好满偏」对应的 duty，保护指针不撞止档。
static uint16_t gDutyCeiling = kPwmRange;

// 表头链路状态
static bool     gMeterLinked = false;   // 是否收到过至少一个 type=1 包
static uint32_t gLastMeterPacketMs = 0;
static uint32_t gMeterPackets = 0;
static uint32_t gMeterLost    = 0;
static uint16_t gLastMeterSeq = 0;
static bool     gMeterSeqValid = false;
static bool     gMeterTimedOut = false;

// 板载 LED 收包指示（低电平点亮）。
// 表头指针动作慢，看不出变化时这盏灯能立刻区分
// 「UDP 没到」还是「表头没反应」。
static uint32_t gLedOffAtMs = 0;

// -------------------------------------------------------------
//  时钟运行时状态
// -------------------------------------------------------------
static uint8_t  gSecRowVariant      = kDefaultSecRowVariant;
static uint8_t  gClockBrightnessIdx = kDefaultClockBrightnessIndex;
static uint32_t gLastClockFrameMs   = 0;
static uint32_t gLastWaitingStepMs  = 0;
static uint32_t gWaitingStep        = 0;
static uint32_t gLastNtpResyncMs    = 0;
static bool     gTimeWasValid       = false;

// -------------------------------------------------------------
//  灯阵统计（沿用阶段 1/2）
// -------------------------------------------------------------
static uint8_t  gBrightness = 32;
static bool     gDirty      = false;

static uint32_t gPackets    = 0;
static uint32_t gLost       = 0;
static uint32_t gFrames     = 0;
static uint32_t gCapped     = 0;
static uint32_t gBadPackets = 0;   // magic / 版本 / 长度不合法（对接上位机时最有用）
static uint16_t gLastSeq    = 0;
static bool     gSeqValid   = false;
static uint16_t gLastMilliAmp = 0;

static uint32_t gLastStatsMs   = 0;
static uint32_t gLastWifiMs    = 0;
static uint32_t gLastMeterTickMs = 0;
static uint32_t gLastMeterStatsMs = 0;

// 主机名 dash-xxxxxx，xxxxxx 取 ChipId 后 6 位十六进制，多机不撞名。
//
// 这个名字同时有三个用途：mDNS 主机名、配网热点的 SSID、串口日志里的标识。
// 三者统一的好处很实在：手机搜到的热点叫 dash-A1B2C3，
// 配完网后上位机 mDNS 看到的也是 dash-A1B2C3 —— 对得上号，不用记两套名字。
static char gHostName[32] = {0};

#if DASHBOARD_ENABLE_MDNS
static IPAddress gMdnsLastIp;      // 上次注册时的 IP，变了要重新注册
static bool     gMdnsStarted = false;
#endif

// ---- 配网模式 ----
static bool     gPortalMode     = false;
static uint32_t gPortalEnterMs  = 0;
static uint32_t gLastPortalFrameMs = 0;
static NetConfig gNetCfg;              // 本次生效的网络配置

// -------------------------------------------------------------
//  按键状态
// -------------------------------------------------------------
static bool     gBtnPrevPressed  = false;
static uint32_t gBtnDownMs       = 0;
static uint32_t gBtnLastChangeMs = 0;
static bool     gBtnHoldPending  = false;   // 正按着，等待判断是否达到「进配网」的时长

// -------------------------------------------------------------
//  像素索引映射  ★不要自行推导★
//    index = (x/8)*64 + (x%8)*8 + (7-y)
// -------------------------------------------------------------
static inline uint16_t xyToIndex(uint8_t x, uint8_t y) {
  return ((uint16_t)(x >> 3) << 6) + ((uint16_t)(x & 0x07) << 3) + (7 - y);
}

// -------------------------------------------------------------
//  帧缓冲操作（★ pushFrame 不得修改 gFrameXY，见函数注释）
// -------------------------------------------------------------
static inline void bufPixel(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= kCols || y >= kRows) return;
  const uint16_t i = (uint16_t)(((uint16_t)y * kCols + x) * 3);
  gFrameXY[i]     = r;
  gFrameXY[i + 1] = g;
  gFrameXY[i + 2] = b;
}

static void bufClear() {
  memset(gFrameXY, 0, sizeof(gFrameXY));
}

static void pushFrame(uint8_t brightness) {
  const uint16_t n = kPixelCount * 3;

  uint32_t sum = 0;
  for (uint16_t i = 0; i < n; ++i) sum += gFrameXY[i];
  sum = (uint32_t)((uint64_t)sum * brightness / 255);

  uint16_t capScale = 256;
  if (sum > kMaxChannelSum) {
    capScale = (uint16_t)(((uint32_t)kMaxChannelSum << 8) / sum);
    if (capScale < 1) capScale = 1;
    ++gCapped;
  }
  const uint32_t actualSum = (sum * capScale) >> 8;
  gLastMilliAmp = (uint16_t)(actualSum * 784 / 10000);

  const uint16_t scale = (uint16_t)(((uint32_t)brightness * capScale) / 255);

  for (uint8_t y = 0; y < kRows; ++y) {
    for (uint8_t x = 0; x < kCols; ++x) {
      const uint16_t i = (uint16_t)(((uint16_t)y * kCols + x) * 3);
      strip.SetPixelColor(xyToIndex(x, y),
          RgbColor((uint8_t)(((uint16_t)gFrameXY[i]     * scale) >> 8),
                   (uint8_t)(((uint16_t)gFrameXY[i + 1] * scale) >> 8),
                   (uint8_t)(((uint16_t)gFrameXY[i + 2] * scale) >> 8)));
    }
  }
  strip.Show();
}

// -------------------------------------------------------------
//  颜色：HSV → RGB
//  参考工程的 wheel() 在色段中间明度掉到 47%，彩虹会一段亮一段暗，
//  这里用标准 HSV→RGB，保持各色相最亮通道恒定。
// -------------------------------------------------------------
static void hsvToRgb(uint8_t h, uint8_t s, uint8_t v,
                     uint8_t& r, uint8_t& g, uint8_t& b) {
  const uint8_t region    = h / 43;
  const uint8_t remainder = (uint8_t)((h - region * 43) * 6);   // 0..255
  const uint16_t sp = (uint16_t)(((uint16_t)s * (uint16_t)remainder) >> 8);
  const uint16_t sq = (uint16_t)(((uint16_t)s * (uint16_t)(255 - remainder)) >> 8);
  const uint8_t  p  = (uint8_t)(((uint16_t)v * (uint16_t)(255 - s)) >> 8);
  const uint8_t  q  = (uint8_t)(((uint16_t)v * (uint16_t)(255 - sp))  >> 8);
  const uint8_t  t  = (uint8_t)(((uint16_t)v * (uint16_t)(255 - sq))  >> 8);
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

// 第 x 列的彩虹色：色相沿横向铺开 kRainbowSpanDeg，整体按 baseDeg 滚动。
// 用彩虹色而不是单色，是用户选定的方案（docs/06 §6.6）。
static void rainbowAt(uint8_t x, uint16_t baseDeg, uint8_t v,
                      uint8_t& r, uint8_t& g, uint8_t& b) {
  uint32_t deg = (uint32_t)baseDeg + ((uint32_t)x * kRainbowSpanDeg) / (kCols - 1);
  deg %= 360;
  const uint8_t h = (uint8_t)((deg * 255UL) / 360UL);
  hsvToRgb(h, 255, v, r, g, b);
}

// -------------------------------------------------------------
//  数字字模：4×7 窄体，MSB = 最左列（bit3）
//  32 列要放 6 位数字 + 2 个冒号，5 宽字模需要 37 列放不下，所以用 4 宽。
//  每位平均点亮 12.6 颗。
// -------------------------------------------------------------
static const uint8_t kDigitFont[10][7] = {
  {0x6, 0x9, 0x9, 0x9, 0x9, 0x9, 0x6},   // 0  14 颗
  {0x2, 0x6, 0x2, 0x2, 0x2, 0x2, 0x7},   // 1  10 颗
  {0x6, 0x9, 0x1, 0x2, 0x4, 0x8, 0xF},   // 2  12 颗
  {0x6, 0x9, 0x1, 0x3, 0x1, 0x9, 0x6},   // 3  12 颗
  {0x1, 0x3, 0x5, 0x9, 0xF, 0x1, 0x1},   // 4  13 颗
  {0xF, 0x8, 0xE, 0x1, 0x1, 0x9, 0x6},   // 5  14 颗
  {0x6, 0x8, 0xE, 0x9, 0x9, 0x9, 0x6},   // 6  14 颗
  {0xF, 0x1, 0x2, 0x4, 0x8, 0x8, 0x8},   // 7  10 颗
  {0x6, 0x9, 0x9, 0x6, 0x9, 0x9, 0x6},   // 8  14 颗
  {0x6, 0x9, 0x9, 0x7, 0x1, 0x2, 0xC},   // 9  13 颗
};

// "WAITING" 的字母，5×7，MSB = 最左列（bit4）。顺序 W A I T N G
static const uint8_t kWaitingGlyphs[6][7] = {
  {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},   // W
  {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},   // A
  {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F},   // I
  {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},   // T
  {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},   // N
  {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E},   // G
};

// 画一个 4×7 数字，逐像素取彩虹色（色相按列展开）
static void drawDigit4(uint8_t digit, int16_t x0, int16_t y0,
                       uint16_t baseDeg, uint8_t v) {
  for (uint8_t row = 0; row < 7; ++row) {
    const uint8_t bits = kDigitFont[digit][row];
    for (uint8_t col = 0; col < kDigitW; ++col) {
      if (!(bits & (0x08 >> col))) continue;
      uint8_t r, g, b;
      rainbowAt((uint8_t)(x0 + col), baseDeg, v, r, g, b);
      bufPixel(x0 + col, y0 + row, r, g, b);
    }
  }
}

// 画一个 5×7 字形（WAITING 用，单色）
static void drawGlyph5(const uint8_t glyph[7], int16_t x0, int16_t y0,
                       uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t row = 0; row < 7; ++row) {
    uint8_t bits = glyph[row];
    for (uint8_t col = 0; col < 5; ++col) {
      if (bits & (0x10 >> col)) bufPixel(x0 + col, y0 + row, r, g, b);
    }
  }
}

// -------------------------------------------------------------
//  底行（y = kProgressBarRow）的秒进度样式
//  时钟改成 HH:MM:SS 后 32 列全被数字占满，秒的"动感"由这一行承担。
// -------------------------------------------------------------
static void drawSecRow(float secFloat, uint16_t baseDeg, uint8_t vMax) {
  switch ((SecRowVariant)gSecRowVariant) {
    case SECROW_OFF:
      // 什么都不画，底行全黑。最省电。
      break;

    case SECROW_TICKS: {
      // 每 5 秒一个刻度点；当前所处的 5 秒区间加亮。
      const uint8_t cur = (uint8_t)(secFloat / 5.0f);        // 0..11
      for (uint8_t x = 0; x < kCols; ++x) {
        const uint8_t tick = (uint8_t)(x * 12 / kCols);       // 该列属于哪个刻度
        const bool on = (tick <= cur);
        const float f = (tick == cur) ? 0.85f : (on ? 0.22f : 0.06f);
        uint8_t r, g, b;
        rainbowAt(x, baseDeg, (uint8_t)((float)vMax * f), r, g, b);
        bufPixel((int16_t)x, kProgressBarRow, r, g, b);
      }
      break;
    }

    case SECROW_BAR:
    default: {
      // 0..59 秒映射到 32 px，走过部分由暗到亮，未走部分留一层极暗底。
      for (uint8_t x = 0; x < kCols; ++x) {
        const float need = (float)x * 60.0f / (float)kCols;
        uint8_t r, g, b;
        if (secFloat >= need) {
          const uint8_t v = (uint8_t)((0.35f + 0.45f * (float)x / (float)kCols) * (float)vMax);
          rainbowAt(x, baseDeg, v, r, g, b);
        } else {
          rainbowAt(x, baseDeg, (uint8_t)((float)vMax * 0.05f), r, g, b);
        }
        bufPixel((int16_t)x, kProgressBarRow, r, g, b);
      }
      break;
    }
  }
}

// -------------------------------------------------------------
//  CLOCK 画面
// -------------------------------------------------------------
static void drawClock(uint32_t nowMs, uint8_t vMax) {
  bufClear();

  const uint16_t baseDeg = (uint16_t)(((nowMs % kRainbowCycleMs) * 360UL) / kRainbowCycleMs);

  const time_t t = time(nullptr);
  struct tm* tmv = localtime(&t);
  if (!tmv) return;

  // ---- HH:MM:SS（24 小时制）----
  const uint8_t digits[kDigitCount] = {
    (uint8_t)(tmv->tm_hour / 10),
    (uint8_t)(tmv->tm_hour % 10),
    (uint8_t)(tmv->tm_min  / 10),
    (uint8_t)(tmv->tm_min  % 10),
    (uint8_t)(tmv->tm_sec  / 10),
    (uint8_t)(tmv->tm_sec  % 10)
  };
  for (uint8_t d = 0; d < kDigitCount; ++d) {
    drawDigit4(digits[d], (int16_t)kDigitX[d], (int16_t)kClockTop, baseDeg, vMax);
  }

  // ---- 两个冒号：白色，不参与彩虹染色 ----
  // 秒已经用数字显示了，冒号常亮比闪烁更稳（闪烁反而会让人误以为时钟在重启）。
  const uint8_t cv = vMax > 200 ? 200 : (vMax < 60 ? 60 : vMax);
  for (uint8_t i = 0; i < 2; ++i) {
    bufPixel(kColonX[i], 2, cv, cv, cv);
    bufPixel(kColonX[i], 4, cv, cv, cv);
  }

  // ---- 底行秒进度 ----
  drawSecRow((float)tmv->tm_sec, baseDeg, vMax);
}

// -------------------------------------------------------------
//  WAITING 画面：横向滚动文字，琥珀色单色（异常态，不用彩虹）
// -------------------------------------------------------------
static void drawWaiting(uint8_t vMax) {
  bufClear();

  const uint8_t glyphCount = 6;
  const uint16_t textW = (uint16_t)(glyphCount * 6);          // 5 + 1 间距
  const uint16_t period = textW + kCols;
  const int16_t off = (int16_t)kCols - (int16_t)(gWaitingStep % period);

  const uint8_t r = (uint8_t)((uint16_t)kWaitingR * vMax / 255);
  const uint8_t g = (uint8_t)((uint16_t)kWaitingG * vMax / 255);
  const uint8_t b = (uint8_t)((uint16_t)kWaitingB * vMax / 255);

  for (uint8_t i = 0; i < glyphCount; ++i) {
    drawGlyph5(kWaitingGlyphs[i], off + (int16_t)(i * 6), kClockTop, r, g, b);
  }
}

// -------------------------------------------------------------
//  CONFIG 画面：琥珀色 "SETUP" 呼吸
// -------------------------------------------------------------
// 配网时灯阵唯一的任务就是告诉用户「我在等人来配网」。
// 用呼吸而不是常亮：常亮的静止画面在灯阵上和「死机」长得一样。
static const uint8_t kSetupGlyphs[5][7] = {
  {0x0E, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},   // S
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},   // E
  {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},   // T
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},   // U
  {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},   // P
};

// 三角波呼吸：0→1→0，周期 kPortalBreathMs
static uint8_t portalBreathV(uint32_t now) {
  const uint32_t half = kPortalBreathMs / 2;
  const uint32_t p    = now % kPortalBreathMs;
  const float f = (p < half) ? ((float)p / (float)half)
                             : (1.0f - (float)(p - half) / (float)half);
  return (uint8_t)(kPortalBreathMin + (float)(kPortalBreathMax - kPortalBreathMin) * f);
}

static void drawSetup(uint8_t v) {
  bufClear();
  const uint8_t r = (uint8_t)((uint16_t)180 * v / 255);
  const uint8_t g = (uint8_t)((uint16_t)100 * v / 255);
  const uint8_t b = (uint8_t)((uint16_t)20  * v / 255);
  // 5 个 5 宽字形 + 4 个 1 px 间距 = 29 列，居中落在 x=1
  for (uint8_t i = 0; i < 5; ++i) {
    drawGlyph5(kSetupGlyphs[i], 1 + (int16_t)(i * 6), kClockTop, r, g, b);
  }
}

// -------------------------------------------------------------
//  表头：PWM 输出
// -------------------------------------------------------------
static void writeMeter(uint8_t ch, uint16_t duty) {
  // 限幅到「硬件允许的安全上限」。串联电阻偏小时它被压到远低于 kPwmRange，
  // 保证指针最多走到机械满偏，不会撞死在止档（那是不可逆的游丝变形）。
  if (duty > gDutyCeiling) duty = gDutyCeiling;
  analogWrite(gMeters[ch].pin, duty);
  gMeters[ch].lastDuty = duty;
}

static void initMeters() {
  // ★ 必须显式设置：core 3.x 的 analogWriteRange 默认是 255，不是 1023。
  // ★ 顺序：先 range/freq，再 pinMode/输出，否则已有波形会被停掉。
  analogWriteRange(kPwmRange);
  analogWriteFreq(kPwmFreqHz);

  for (uint8_t i = 0; i < kMeterCount; ++i) {
    gMeters[i].pin     = kMeterPins[i];
    gMeters[i].trim    = kMeterTrim[i];
    gMeters[i].target  = 0.0f;
    gMeters[i].display = 0.0f;
    gMeters[i].lastDuty = 0;
    pinMode(gMeters[i].pin, OUTPUT);
    analogWrite(gMeters[i].pin, 0);
  }
}

// 100% 占空比时的回路电流（µA）：3.3 V / 总阻
static inline float fullScaleCurrentUa() {
  return 3300000.0f / (kMeterSeriesOhm + kMeterInternalOhm);
}

// 电参数自检。限流电阻选错时这里会直接说清楚，不用对着指针猜。
static void printMeterElectricalCheck() {
  const float ua  = fullScaleCurrentUa();
  const float pct = ua / kMeterFullScaleUa * 100.0f;

  Serial.print(F("[Meter] 串联 "));
  Serial.print(kMeterSeriesOhm / 1000.0f, 1);
  Serial.print(F(" kΩ + 内阻 "));
  Serial.print(kMeterInternalOhm / 1000.0f, 1);
  Serial.print(F(" kΩ  →  100% 占空比电流 "));
  Serial.print(ua, 2);
  Serial.print(F(" µA  = 额定 "));
  Serial.print(kMeterFullScaleUa, 0);
  Serial.print(F(" µA 的 "));
  Serial.print(pct, 1);
  Serial.println('%');
  if (!kMeterAutoClamp) {
    Serial.println(F("[Meter] 自动限幅已关闭：占空比 0~1023 全量程直通，不做任何压制。"));
    return;
  }

  if (pct > 105.0f) {
    const uint16_t safe = (uint16_t)(kMeterFullScaleUa / ua * (float)kPwmRange);
    gDutyCeiling = safe < 1 ? 1 : safe;

    Serial.println(F("[Meter] ⚠️ 过载 —— 指针会撞死在机械止档："));
    Serial.print(F("        有效行程只占 0 ~ "));
    Serial.print(kMeterFullScaleUa / ua * 100.0f, 1);
    Serial.println(F("%，再往上纯属浪费且会让游丝永久变形"));
    Serial.print(F("        ▶ 已启用限幅 duty≤"));
    Serial.print(gDutyCeiling);
    Serial.println(F("，指针最多走到机械满偏，不会撞死"));
    Serial.println(F("        根治：串联电阻换成 20 kΩ（100% 行程）"));
  } else if (pct < 70.0f) {
    Serial.print(F("[Meter] ⚠️ 行程不足 —— 满偏只到 "));
    Serial.print(pct, 1);
    Serial.println(F("%，换小一点的串联电阻"));
  } else {
    Serial.print(F("[Meter] 正常，可用行程约 "));
    Serial.print(pct, 1);
    Serial.println('%');
  }
}

// 上电扫针自检：5 路同时 0 → 满偏 → 0。
// 目的是在连 WiFi 之前先确认硬件通道都通、指针没卡死 ——
// 纯硬件阶段失败，排错范围小得多。
static void runNeedleSweep() {
  Serial.println(F("[Meter] 扫针自检  0% → 100% → 0%（5 路同时）"));

  const uint32_t total = kSweepRampMs + kSweepHoldMs + kSweepDownMs;
  const uint32_t t0 = millis();
  uint32_t lastPrint = 0;

  while (millis() - t0 < total) {
    const uint32_t e = millis() - t0;
    uint16_t duty;
    if (e < kSweepRampMs) {
      duty = (uint16_t)((uint32_t)e * gDutyCeiling / kSweepRampMs);
    } else if (e < kSweepRampMs + kSweepHoldMs) {
      duty = gDutyCeiling;
    } else {
      const uint32_t d = e - kSweepRampMs - kSweepHoldMs;
      duty = (uint16_t)(gDutyCeiling - (uint32_t)d * gDutyCeiling / kSweepDownMs);
    }
    for (uint8_t i = 0; i < kMeterCount; ++i) writeMeter(i, duty);

    if (e - lastPrint >= 200) {
      lastPrint = e;
      Serial.print(F("          占空比 "));
      Serial.print(duty);
      Serial.print(F(" / "));
      Serial.print(gDutyCeiling);
      Serial.print(F("   ("));
      Serial.print((float)duty * 100.0f / (float)kPwmRange, 1);
      Serial.println(F("%)"));
    }
    yield();
    delay(5);
  }

  for (uint8_t i = 0; i < kMeterCount; ++i) {
    writeMeter(i, 0);
    gMeters[i].target  = 0.0f;
    gMeters[i].display = 0.0f;
  }
  Serial.println(F("[Meter] 扫针完成"));
}

// -------------------------------------------------------------
//  表头：目标值来源
// -------------------------------------------------------------
// 演示三角波（只在从未收到过表头包时跑）。
// 5 路相位错开 1/5 周期，方便同时观察多路是否都动。
static float demoTriangle(uint8_t ch, uint32_t now) {
  const uint32_t half = kDemoPeriodMs / 2;
  const uint32_t phase = (now + (uint32_t)ch * (kDemoPeriodMs / kMeterCount)) % kDemoPeriodMs;
  float f;
  if (phase < half) f = (float)phase / (float)half;
  else              f = 1.0f - (float)(phase - half) / (float)half;
  return f * (float)kPwmRange;
}

// 100 Hz 渲染节拍。
static void tickMeters(uint32_t now) {
  const uint32_t dt = now - gLastMeterTickMs;
  if (dt < kMeterTickMs) return;
  gLastMeterTickMs = now;

  // 1) 决定本拍的目标值
  if (!gMeterLinked) {
    // 还没见过上位机 → 演示图案
    for (uint8_t i = 0; i < kMeterCount; ++i)
      gMeters[i].target = demoTriangle(i, now);
  } else if (now - gLastMeterPacketMs > kMeterTimeoutMs) {
    // 超时 → 回到演示图案（不瞬跳，缓动过渡到三角波）
    if (!gMeterTimedOut) {
      gMeterTimedOut = true;
      Serial.println(F("[Meter] 超时 → 演示模式"));
    }
    for (uint8_t i = 0; i < kMeterCount; ++i)
      gMeters[i].target = demoTriangle(i, now);
  }

  // 2) 指数缓动
  //    alpha 按实际 dt 缩放：主循环被 DMA / WiFi 拖慢时，
  //    时间常数 τ 保持不变，手感不会随负载漂移。
  float alpha = kMeterAlpha * (float)dt / (float)kMeterTickMs;
  if (alpha > 1.0f) alpha = 1.0f;

  for (uint8_t i = 0; i < kMeterCount; ++i) {
    MeterCh &m = gMeters[i];
    m.display += (m.target - m.display) * alpha;
    // 吸附：足够接近就直接到位，否则 duty 会在末位反复抖动
    if (fabsf(m.target - m.display) < 0.5f) m.display = m.target;
    const uint16_t duty = (uint16_t)(m.display + 0.5f);
    if (duty != m.lastDuty) writeMeter(i, duty);
  }
}

// -------------------------------------------------------------
//  时间
// -------------------------------------------------------------
static bool isTimeValid() {
  return (uint32_t)time(nullptr) > kValidUnixThreshold;
}

static void startNtp() {
  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  Serial.print(F("[NTP] 已启动  TZ="));
  Serial.print(F(TZ_STRING));
  Serial.print(F("  服务器 "));
  Serial.print(F(NTP_SERVER_1));
  Serial.print(F(" / "));
  Serial.print(F(NTP_SERVER_2));
  Serial.print(F(" / "));
  Serial.println(F(NTP_SERVER_3));
}

static void printClockInfo() {
  if (!isTimeValid()) {
    Serial.println(F("[Clock] 时间未同步（将在 WAITING 状态滚动显示）"));
    return;
  }
  const time_t t = time(nullptr);
  struct tm* tmv = localtime(&t);
  if (!tmv) return;
  Serial.print(F("[Clock] 已同步  本地时间 "));
  if (tmv->tm_hour < 10) Serial.print('0');
  Serial.print(tmv->tm_hour);
  Serial.print(':');
  if (tmv->tm_min < 10) Serial.print('0');
  Serial.print(tmv->tm_min);
  Serial.print(':');
  if (tmv->tm_sec < 10) Serial.print('0');
  Serial.println(tmv->tm_sec);
}

// -------------------------------------------------------------
//  UDP
// -------------------------------------------------------------
//  ★ now 由 loop() 传入，包内的所有时间戳都必须用它，绝不能再自己调 millis()。
//    理由：loop() 开头取的 now 是整个 Loop 的统一时基，UDP 收包发生在它之后；
//    如果这里另调 millis()，得到的值会 > now，于是 withinTimeout 里
//    `now - lastMs` 无符号下溢成巨大值 → 每收一个包就误判超时一次 → 画面闪烁。
//    （2026-09-26 实测证实：推流时 [State] 在 LIVE/CLOCK 间 10~50 ms 成对抖动）
static void handlePacket(const uint8_t* p, int len, uint32_t now) {
  // 非法包一律计数而不是静默丢弃。
  // 上位机对接时最常见的失败是「版本号写错 / magic 写反 / 长度算错」，
  // 这类包如果不留痕迹，现象就是毫无征兆的黑屏，极难排查。
  if (len < kHeaderBytes)              { ++gBadPackets; return; }
  if (p[0] != kMagic0 || p[1] != kMagic1) { ++gBadPackets; return; }
  if (p[2] != kProtoVersion)           { ++gBadPackets; return; }

  const uint8_t  type = p[3];
  const uint16_t seq  = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));   // 小端

  // ---- type=1 · 表头值（18 B）----
  if (type == kTypeMeters) {
    if (len < kMetersPacketLen) return;

    ++gMeterPackets;
    if (gMeterSeqValid) {
      const uint16_t gap = (uint16_t)(seq - gLastMeterSeq);
      if (gap > 1) gMeterLost += (uint32_t)(gap - 1);
    }
    gLastMeterSeq  = seq;
    gMeterSeqValid = true;

    for (uint8_t i = 0; i < kMeterCount; ++i) {
      const uint16_t v = (uint16_t)(p[kHeaderBytes + i * 2] |
                                   ((uint16_t)p[kHeaderBytes + i * 2 + 1] << 8));
      float norm = (float)v / 65535.0f * gMeters[i].trim;
      if (norm > 1.0f) norm = 1.0f;              // trim>1 也不会过载
      gMeters[i].target = norm * (float)kPwmRange;
    }

    gLastMeterPacketMs = now;
    if (gMeterTimedOut) Serial.println(F("[Meter] 恢复接收"));
    gMeterTimedOut = false;
    if (!gMeterLinked) {
      gMeterLinked = true;
      Serial.println(F("[Meter] 已接管表头（演示图案停止）"));
    }

    // 收包指示：低电平点亮 30 ms
    digitalWrite(LED_BUILTIN, LOW);
    gLedOffAtMs = now + 30;
    return;
  }

  // ---- type=2 · 像素帧（776 B）----
  if (type == kTypePixels) {
    if (len < kPixelsPacketLen) return;

    ++gPackets;
    if (gSeqValid) {
      const uint16_t gap = (uint16_t)(seq - gLastSeq);
      if (gap > 1) gLost += (uint32_t)(gap - 1);
    }
    gLastSeq  = seq;
    gSeqValid = true;

    memcpy(gFrameXY, p + kHeaderBytes, kPixelPayloadBytes);
    gBrightness = (p[6] > kMaxBrightness) ? kMaxBrightness : p[6];
    gDirty         = true;
    gLastPixelPacketMs = now;
    return;
  }

  // ---- type=3 · 授时（12 B，可选）----
  // 只在偏差明显时才纠正，否则会和 NTP 来回拉扯。
  if (type == kTypeClock) {
    if (len < kClockPacketLen) { ++gBadPackets; return; }
    const uint32_t remote = (uint32_t)p[kHeaderBytes] |
                            ((uint32_t)p[kHeaderBytes + 1] << 8) |
                            ((uint32_t)p[kHeaderBytes + 2] << 16) |
                            ((uint32_t)p[kHeaderBytes + 3] << 24);
    const uint32_t local = (uint32_t)time(nullptr);
    const uint32_t diff = remote > local ? remote - local : local - remote;
    if (remote > kValidUnixThreshold && diff > kTimeCorrectThresholdSec) {
      struct timeval tv;
      tv.tv_sec  = (time_t)remote;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      Serial.print(F("[Clock] 上位机授时校正，偏差 "));
      Serial.print(diff);
      Serial.println(F(" s"));
    }
    return;
  }

  // 走到这里说明 type 是未知值（≥4）
  ++gBadPackets;
}

// latest-wins 的关键：必须把 socket 读空。
static void pumpUdp(uint32_t now) {
  int sz;
  while ((sz = gUdp.parsePacket()) > 0) {
    if (sz > (int)sizeof(gRxBuf)) {
      ++gBadPackets;
      gUdp.flush();
      continue;
    }
    const int n = gUdp.read(gRxBuf, sz);
    if (n > 0) handlePacket(gRxBuf, n, now);
  }
}

// -------------------------------------------------------------
//  mDNS 服务发现
//  注册 _dashboard._udp，上位机 browse 就能找到，无需知道 IP。
//  验证命令（不用开上位机）：dns-sd -B _dashboard._udp
// -------------------------------------------------------------
#if DASHBOARD_ENABLE_MDNS
// TXT 记录：静态元数据，上位机列设备时直接显示，省得以后再扩协议。
// 只放「不常变」的字段；fps / 丢包这类实时值以后用 type=4 查询。
static void startMdns() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFi.hostname(gHostName);

  if (!MDNS.begin(gHostName)) {
    Serial.println(F("[mDNS] 启动失败（多播可能被网络拦截）"));
    return;
  }

  MDNS.addService(kMdnsServiceType, "udp", kUdpPort);

  char buf[24];
  MDNS.addServiceTxt(kMdnsServiceType, "udp", "ver", kFirmwareVersion);
  snprintf(buf, sizeof(buf), "%u", (unsigned)kProtoVersion);
  MDNS.addServiceTxt(kMdnsServiceType, "udp", "proto", buf);
  MDNS.addServiceTxt(kMdnsServiceType, "udp", "mac", WiFi.macAddress().c_str());
  snprintf(buf, sizeof(buf), "%u", (unsigned)kPixelCount);
  MDNS.addServiceTxt(kMdnsServiceType, "udp", "leds", buf);
  snprintf(buf, sizeof(buf), "%u", (unsigned)kMeterCount);
  MDNS.addServiceTxt(kMdnsServiceType, "udp", "meters", buf);
  snprintf(buf, sizeof(buf), "%u", (unsigned)kPixelCount / 8);
  MDNS.addServiceTxt(kMdnsServiceType, "udp", "cols", buf);

  gMdnsLastIp  = WiFi.localIP();
  gMdnsStarted = true;

  Serial.print(F("[mDNS] 已注册 "));
  Serial.print(gHostName);
  Serial.print(F(".local -> _"));
  Serial.print(kMdnsServiceType);
  Serial.print(F("._udp  port="));
  Serial.println(kUdpPort);
}

// WiFi 重连后 IP 可能变，mDNS 的 A 记录会指向旧地址 → 必须重新注册。
static void pollMdns() {
  if (WiFi.status() != WL_CONNECTED) {
    gMdnsStarted = false;
    return;
  }
  if (!gMdnsStarted) {
    startMdns();
    return;
  }
  if (WiFi.localIP() != gMdnsLastIp) {
    Serial.print(F("[mDNS] IP 变更 "));
    Serial.print(gMdnsLastIp);
    Serial.print(F(" -> "));
    Serial.print(WiFi.localIP());
    Serial.println(F(" ，重新注册"));
    startMdns();
    return;
  }
  MDNS.update();   // 处理来自上位机的查询
}
#endif

// -------------------------------------------------------------
//  WiFi
// -------------------------------------------------------------
// 主机名 = 前缀 + ChipId 后 6 位十六进制。
// 用 ChipId 而不是手写字符串，是为了多机同网时不撞名。
// 注意它不依赖 mDNS 开关 —— 配网热点的 SSID 也要用它。
static void buildHostName() {
  const uint32_t id = ESP.getChipId();
  snprintf(gHostName, sizeof(gHostName), "%s%06X", kMdnsHostPrefix, (unsigned)(id & 0xFFFFFFu));
}

// -------------------------------------------------------------
//  去哪儿拿 WiFi 配置
//
//  优先级：EEPROM（手机配过） > secrets.h（编译期兜底） > 没有
//
//  secrets.h 这一层只是为了让「烧录时顺手填过一次」的老流程继续可用。
//  它是不是真的填了，靠比对模板占位符判断 —— 占位符没改就当没配，
//  直接进配网门户，而不是傻等一个永远连不上的 SSID。
// -------------------------------------------------------------
static bool resolveNetworkConfig(NetConfig& out) {
  netConfigReset(out);

  if (netConfigLoad(out)) {
    Serial.print(F("[Net] 用 EEPROM 里的配置  SSID="));
    Serial.print(out.ssid);
    Serial.println(out.flags & kNetFlagStaticIp ? F("  (静态 IP)") : F("  (DHCP)"));
    return true;
  }

  if (strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_2.4G_SSID") != 0) {
    strncpy(out.ssid, WIFI_SSID, kNetSsidMax);
    strncpy(out.pass, WIFI_PASSWORD, kNetPassMax);
#if USE_STATIC_IP
    out.flags |= kNetFlagStaticIp;
    out.ip[0]   = MODULE_IP[0];    out.ip[1]   = MODULE_IP[1];
    out.ip[2]   = MODULE_IP[2];    out.ip[3]   = MODULE_IP[3];
    out.gw[0]   = GATEWAY_IP[0];   out.gw[1]   = GATEWAY_IP[1];
    out.gw[2]   = GATEWAY_IP[2];   out.gw[3]   = GATEWAY_IP[3];
    out.mask[0] = SUBNET_MASK[0];  out.mask[1] = SUBNET_MASK[1];
    out.mask[2] = SUBNET_MASK[2];  out.mask[3] = SUBNET_MASK[3];
    out.dns[0]  = DNS_IP[0];       out.dns[1]  = DNS_IP[1];
    out.dns[2]  = DNS_IP[2];       out.dns[3]  = DNS_IP[3];
#endif
    Serial.print(F("[Net] EEPROM 无配置，回退到 secrets.h 的 SSID="));
    Serial.println(out.ssid);
    Serial.println(F("       （换 WiFi 还是要重新烧录，建议改用手机配网）"));
    return true;
  }

  return false;   // 没有任何可用配置 → 交给配网门户
}

static bool connectWifi(const NetConfig& c) {
  Serial.print(F("[WiFi] 连接 "));
  Serial.print(c.ssid);
  Serial.print(F(" ... "));

  WiFi.mode(WIFI_STA);
  // ★ 关掉 SDK 自己的持久化：它也会往 flash 写 WiFi 凭证，
  //   既加速损耗又和我们的 EEPROM 配置打架（改了页面等于没改）。
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);

  if (c.flags & kNetFlagStaticIp) {
    WiFi.config(IPAddress(c.ip[0],   c.ip[1],   c.ip[2],   c.ip[3]),
                IPAddress(c.gw[0],   c.gw[1],   c.gw[2],   c.gw[3]),
                IPAddress(c.mask[0], c.mask[1], c.mask[2], c.mask[3]),
                IPAddress(c.dns[0],  c.dns[1],  c.dns[2],  c.dns[3]));
  } else {
    // ★ 全 0 表示「交回 DHCP」。这一句不能省：
    //   上一份配置是静态 IP 的话，SDK 会一直沿用旧地址，
    //   现象是「页面上明明选了 DHCP，重启后还是老 IP」。
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  }

  WiFi.begin(c.ssid, c.pass);

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kWifiConnectTimeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] 已连接  IP="));
    Serial.print(WiFi.localIP());
    Serial.print(F("  RSSI="));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));
    // 主机名是上位机 Browse 时看到的实例名，烧录后照这个 ping/排查
    Serial.print(F("[WiFi] 主机名 "));
    Serial.println(gHostName);
    return true;
  }

  Serial.println(F("[WiFi] 连接失败"));
  return false;
}

// -------------------------------------------------------------
//  进入配网模式
// -------------------------------------------------------------
static void enterPortal() {
#if DASHBOARD_ENABLE_PORTAL
  if (gPortalMode) return;

  Serial.println(F("[Portal] 进入配网模式"));
  // softAP 起不来的情况极少（多半是内存不足）。此时不进配网模式，
  // 继续走下面的 WiFi 重试逻辑，免得卡在一个什么都做不了的半死状态。
  if (!portalStart(gHostName)) {
    Serial.println(F("[Portal] 热点启动失败 → 继续走 WiFi 重试，稍后可再长按按键试一次"));
    return;
  }
  gPortalMode        = true;
  gPortalEnterMs     = millis();
  gLastPortalFrameMs = millis();
  gState             = ST_CONFIG;
  Serial.print(F("[Portal] 手机连接热点 "));
  Serial.print(portalApSsid());
  Serial.print(F(" ，然后浏览器打开 http://"));
  Serial.println(portalApIp());
#else
  Serial.println(F("[Portal] 未启用（config.h 的 DASHBOARD_ENABLE_PORTAL=0）"));
#endif
}

static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - gLastWifiMs < kWifiRetryMs) return;
  gLastWifiMs = now;
  Serial.println(F("[WiFi] 断开，重连中..."));
  WiFi.reconnect();
}

// -------------------------------------------------------------
//  按键：短按切底行秒进度样式，长按切时钟明度
// -------------------------------------------------------------
static const char* secRowName(uint8_t v) {
  switch ((SecRowVariant)v) {
    case SECROW_OFF:   return "关闭";
    case SECROW_TICKS: return "刻度";
    case SECROW_BAR:
    default:           return "进度条";
  }
}

static void handleButton(uint32_t now) {
  const bool pressed = (digitalRead(kButtonPin) == LOW);

  // ★ 超长按（进配网）必须在「状态没变化就返回」之前判断。
  //   那个 return 让按住期间根本进不到下面的分支，
  //   写在里面的话长按到 5 秒也不会有任何反应 —— 只有松手才触发。
  if (pressed && gBtnHoldPending && !gPortalMode && now - gBtnDownMs >= kPortalHoldMs) {
    gBtnHoldPending = false;
    Serial.println(F("[Btn] 超长按 → 进入配网模式"));
    enterPortal();
    return;
  }

  if (pressed == gBtnPrevPressed) return;
  if (now - gBtnLastChangeMs < kButtonDebounceMs) return;
  gBtnLastChangeMs = now;

  if (pressed) {
    gBtnDownMs = now;
    gBtnHoldPending = true;
  } else {
    gBtnHoldPending = false;
    if (now - gBtnDownMs >= kLongPressMs) {
      gClockBrightnessIdx = (uint8_t)((gClockBrightnessIdx + 1) % kClockBrightnessCount);
      Serial.print(F("[Btn] 时钟明度 -> "));
      Serial.println(kClockBrightnessSteps[gClockBrightnessIdx]);
    } else {
      gSecRowVariant = (uint8_t)((gSecRowVariant + 1) % SECROW_COUNT);
      Serial.print(F("[Btn] 底行秒进度 -> "));
      Serial.println(secRowName(gSecRowVariant));
    }
  }
  gBtnPrevPressed = pressed;
}

// -------------------------------------------------------------
//  诊断输出
// -------------------------------------------------------------
static void printPct(float v) {
  if (v < 10.0f)  Serial.print(' ');
  if (v < 100.0f) Serial.print(' ');
  Serial.print(v, 1);
}

static void printDuty(uint16_t d) {
  if (d < 1000) Serial.print(' ');
  if (d < 100)  Serial.print(' ');
  if (d < 10)   Serial.print(' ');
  Serial.print(d);
}

static void printMeterStats() {
  Serial.print(F("[Meter] "));
  Serial.print(!gMeterLinked ? F("DEMO ") : (gMeterTimedOut ? F("TIMEOUT") : F("LIVE ")));

  Serial.print(F("  %: "));
  for (uint8_t i = 0; i < kMeterCount; ++i) {
    // 相对「安全行程上限」的百分比：限幅生效时这里显示的就是实际行程占比
    float d = gMeters[i].display;
    if (d > (float)gDutyCeiling) d = (float)gDutyCeiling;
    printPct(d * 100.0f / (float)gDutyCeiling);
    Serial.print(' ');
  }
  Serial.print(F("  duty: "));
  for (uint8_t i = 0; i < kMeterCount; ++i) {
    printDuty(gMeters[i].lastDuty);
    Serial.print(' ');
  }
  Serial.print(F("  pkt="));
  Serial.print(gMeterPackets);
  Serial.print(F("  loss="));
  const uint32_t tot = gMeterPackets + gMeterLost;
  Serial.print(tot ? (float)gMeterLost * 100.0f / (float)tot : 0.0f, 1);
  Serial.println('%');
}

static const char* stateName(DisplayState s) {
  switch (s) {
    case ST_LIVE:    return "LIVE";
    case ST_CLOCK:   return "CLOCK";
    case ST_CONFIG:  return "CFG";
    case ST_WAITING:
    default:         return "WAIT";
  }
}

static void printStats(uint32_t elapsedMs) {
  Serial.print(F("["));
  Serial.print(stateName(gState));
  Serial.print(F("] fps="));
  Serial.print((float)gFrames * 1000.0f / (float)elapsedMs, 1);
  Serial.print(F("  pkt="));
  Serial.print(gPackets);
  Serial.print(F("  loss="));
  const uint32_t totalPkts = gPackets + gLost;
  Serial.print(totalPkts ? (float)gLost * 100.0f / (float)totalPkts : 0.0f, 1);
  Serial.print(F("%  brt="));
  Serial.print(gBrightness);
  Serial.print(F("  clkV="));
  Serial.print(kClockBrightnessSteps[gClockBrightnessIdx]);
  Serial.print(F("  mA="));
  Serial.print(gLastMilliAmp);
  Serial.print(F("  cap="));
  Serial.print(gCapped);
  Serial.print(F("  bad="));
  Serial.print(gBadPackets);
  Serial.print(F("  secrow="));
  Serial.print(secRowName(gSecRowVariant));
  Serial.print(F("  rssi="));
  Serial.print(WiFi.RSSI());
  Serial.print(F("  heap="));
  Serial.println(ESP.getFreeHeap());

  gPackets    = 0;
  gLost       = 0;
  gFrames     = 0;
  gCapped     = 0;
  gBadPackets = 0;
}

// -------------------------------------------------------------
//  setup / loop
// -------------------------------------------------------------
void setup() {
  // ★ 顺序至关重要：必须先 Serial.begin()，再 strip.Begin()。
  //   否则 Serial 会把 GPIO3 抢回做 RX，DMA 输出完全不生效（现象：灯全不亮）。
  //   注意串口只能输出，收不了命令（RX 被 DMA 占用）。
  Serial.begin(kSerialBaud);
  Serial.println();
  Serial.println(F("=== 阶段3 · NTP 彩虹时钟 + 超时降级 + 5 路表头 PWM ==="));
  Serial.print(F("监听 UDP "));
  Serial.print(kUdpPort);
  Serial.print(F("    PWM "));
  Serial.print(kPwmFreqHz);
  Serial.print(F(" Hz / "));
  Serial.print(kPwmRange + 1);
  Serial.print(F(" 级  缓动 τ≈"));
  Serial.print((float)kMeterTickMs / kMeterAlpha, 0);   // 10ms / 0.05 = 200ms
  Serial.println(F(" ms"));

  Serial.println(F("表头引脚："));
  for (uint8_t i = 0; i < kMeterCount; ++i) {
    Serial.print(F("  ch"));
    Serial.print(i + 1);
    Serial.print(F(" -> GPIO"));
    Serial.print(kMeterPins[i]);
    Serial.println(F("   GPIO → 限流电阻 → 表头(+) → 表头(−) → GND"));
  }
  printMeterElectricalCheck();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);   // 熄灭
  pinMode(kButtonPin, INPUT_PULLUP);

  strip.Begin();
  strip.ClearTo(RgbColor(0, 0, 0));
  strip.Show();

  // 开机黑屏窗口：此刻灯珠已被清成全黑，趁这段时间内点 Upload 烧录，
  // 灯阵就没有电流负载，不必拔线（原理见 config.h 的 kBootBlackoutMs 注释）。
  if (kBootBlackoutMs > 0) {
    Serial.print(F("[Boot] 黑屏窗口 "));
    Serial.print(kBootBlackoutMs);
    Serial.println(F(" ms —— 此刻灯未点亮，趁现在点 Upload 可直接烧录"));
    delay(kBootBlackoutMs);
  }

  initMeters();
  runNeedleSweep();

  buildHostName();   // 要在 connectWifi / portalStart 之前：两处都要用这个名字

  // ---- WiFi：EEPROM 优先，secrets.h 兜底，都没有就进配网 ----
  bool wifiOk = false;
  if (resolveNetworkConfig(gNetCfg)) wifiOk = connectWifi(gNetCfg);

#if DASHBOARD_ENABLE_PORTAL
  if (!wifiOk) enterPortal();
#else
  if (!wifiOk) Serial.println(F("[WiFi] 无可用配置，且配网门户未启用 → 将每 5s 重试"));
#endif

  if (!gPortalMode) {
#if DASHBOARD_ENABLE_MDNS
    startMdns();     // WiFi 已通，立刻注册；后续 IP 变更由 pollMdns 兜底
#endif
    startNtp();
  } else {
    Serial.println(F("[NTP] 配网模式下不同步时间，配完网重启后再说"));
  }

  if (gUdp.begin(kUdpPort)) {
    Serial.print(F("[UDP] 监听端口 "));
    Serial.println(kUdpPort);
  } else {
    Serial.println(F("[UDP] 监听失败"));
  }

  Serial.print(F("时钟格式 HH:MM:SS（4×7 窄体字模）  底行秒进度："));
  Serial.print(secRowName(gSecRowVariant));
  Serial.println(F("（短按 FLASH 键切换，长按切亮度）"));

  const uint32_t now = millis();
  gLastStatsMs       = now;
  gLastWifiMs        = now;
  gLastMeterTickMs   = now;
  gLastMeterStatsMs  = now;
  gLastMeterPacketMs = now;
  gLastWaitingStepMs = now;
  gLastClockFrameMs  = now;
  gLastNtpResyncMs   = now;
  // 初始就当作「已经超时」，否则上电头 5 秒会被误判成 LIVE。
  // 这样写即使 millis() 溢出也成立。
  gLastPixelPacketMs = now - kPixelTimeoutMs - 1;
  gStateEnteredMs    = now;
}

void loop() {
  const uint32_t now = millis();

  // ---- 配网模式：整个 loop 走另一条路 ----
  // 这时候 STA 没连上，UDP / mDNS / NTP 统统没有意义，
  // 只保留表头演示、按键、灯阵提示和门户本身。
  if (gPortalMode) {
    portalLoop();
    tickMeters(now);
    handleButton(now);

    if (now - gLastPortalFrameMs >= kPortalFrameMs && strip.CanShow()) {
      gLastPortalFrameMs = now;
      drawSetup(portalBreathV(now));
      pushFrame(255);   // 明度已经写进像素值里
    }

    if (now - gLastStatsMs >= kStatsIntervalMs) {
      gLastStatsMs = now;
      Serial.print(F("[CFG] 配网中  热点="));
      Serial.print(portalApSsid());
      Serial.print(F("  AP="));
      Serial.print(portalApIp());
      Serial.print(F("  已连设备="));
      Serial.print(WiFi.softAPgetStationNum());
      Serial.print(F("  heap="));
      Serial.println(ESP.getFreeHeap());
    }

    // 热点开了太久没人配 → 重启重试一次。
    // 防的是「路由器临时断电 → 设备开一整天热点没人管」这种情况。
    if (now - gPortalEnterMs >= kPortalTimeoutMs) {
      Serial.println(F("[CFG] 配网超时，重启重试"));
      delay(200);
      ESP.restart();
    }

    yield();
    return;
  }

  pumpUdp(now);
  ensureWifi();
#if DASHBOARD_ENABLE_MDNS
  pollMdns();
#endif

  // ---- 表头：100 Hz 缓动渲染（与灯阵状态无关，独立运行）----
  tickMeters(now);
  handleButton(now);

  // ---- 收包指示 LED ----
  if (gLedOffAtMs && (int32_t)(now - gLedOffAtMs) >= 0) {
    digitalWrite(LED_BUILTIN, HIGH);
    gLedOffAtMs = 0;
  }

  // ---- NTP 兜底重同步：每 2 小时强制刷一次 ----
  if (now - gLastNtpResyncMs >= kNtpResyncMs) {
    gLastNtpResyncMs = now;
    startNtp();
  }
  // 时间从无效变有效时立刻回报一次
  const bool tv = isTimeValid();
  if (tv && !gTimeWasValid) {
    gTimeWasValid = true;
    printClockInfo();
  }

  // ---- 三态机：LIVE > CLOCK > WAITING ----
  DisplayState st;
  if (withinTimeout(gLastPixelPacketMs, now, kPixelTimeoutMs)) st = ST_LIVE;
  else if (tv)                                                 st = ST_CLOCK;
  else                                                         st = ST_WAITING;

  if (st != gState) {
    gState         = st;
    gStateEnteredMs = now;
    Serial.print(F("[State] -> "));
    Serial.println(stateName(st));
    if (st == ST_CLOCK) printClockInfo();
  }

  switch (gState) {
    // ST_CONFIG 走不到这里：loop 开头已经 return 了。留着只为消除 -Wswitch 警告。
    case ST_CONFIG:
      break;

    case ST_LIVE:
      // 上位机画面优先。注意这里不做任何插值，来一帧推一帧。
      if (gDirty && strip.CanShow()) {
        pushFrame(gBrightness);
        ++gFrames;
        gDirty = false;
      }
      break;

    case ST_CLOCK:
    case ST_WAITING: {
      if (now - gLastClockFrameMs >= kClockFrameMs && strip.CanShow()) {
        gLastClockFrameMs = now;

        // 淡入：状态切换后 V 从 0 渐升到目标值，避免画面硬切
        uint8_t targetV = kClockBrightnessSteps[gClockBrightnessIdx];
        const uint32_t since = now - gStateEnteredMs;
        if (since < kClockFadeInMs) {
          targetV = (uint8_t)((uint16_t)targetV * since / kClockFadeInMs);
          if (targetV == 0) targetV = 1;
        }

        if (gState == ST_CLOCK) {
          drawClock(now, targetV);
        } else {
          if (now - gLastWaitingStepMs >= kWaitingStepMs) {
            gLastWaitingStepMs = now;
            ++gWaitingStep;
          }
          drawWaiting(targetV);
        }
        pushFrame(255);   // 明度已经写进像素值里，这里走全量
        ++gFrames;
      }
      break;
    }
  }

  if (now - gLastMeterStatsMs >= kMeterStatsMs) {
    printMeterStats();
    gLastMeterStatsMs = now;
  }

  if (now - gLastStatsMs >= kStatsIntervalMs) {
    printStats(now - gLastStatsMs);
    gLastStatsMs = now;
  }

  yield();
}
