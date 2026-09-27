// =============================================================
//  阶段 2 · UDP 收包 + 灯阵渲染 + 5 路表头 PWM
//
//  在阶段 1 的基础上叠加表头：收 type=1 包 → 100 Hz 指数缓动 → PWM 输出。
//  仍未做 OTA / NTP 时钟 / 超时切彩虹时钟 —— 那些在后续阶段叠加。
//
//  映射公式已由阶段 0 实测确认，见 docs/03-matrix-mapping.md
//  表头驱动见 docs/02-meter-driver.md（§2.11 为实际接法核算）
//  协议见 docs/04-udp-protocol.md，功耗见 docs/05-power-budget.md
// =============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NeoPixelBus.h>
#include "config.h"

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

// -------------------------------------------------------------
//  灯总线
//  I2S DMA 固定占用 GPIO3 (RXD0)，构造时的 pin 参数会被忽略。
// -------------------------------------------------------------
NeoPixelBus<NeoGrbFeature, NeoEsp8266Dma800KbpsMethod> strip(kPixelCount);

static uint8_t gFrameXY[kPixelCount * 3];
static uint8_t gRxBuf[kRxBufferBytes];

static WiFiUDP gUdp;

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
//  灯阵运行时状态（沿用阶段 1）
// -------------------------------------------------------------
static uint8_t  gBrightness = 32;
static bool     gDirty      = false;
static bool     gLinked     = false;

static uint32_t gPackets    = 0;
static uint32_t gLost       = 0;
static uint32_t gFrames     = 0;
static uint32_t gCapped     = 0;
static uint16_t gLastSeq    = 0;
static bool     gSeqValid   = false;
static uint16_t gLastMilliAmp = 0;

static uint32_t gLastStatsMs   = 0;
static uint32_t gLastStandbyMs = 0;
static uint32_t gLastWifiMs    = 0;
static uint32_t gLastMeterTickMs = 0;
static uint32_t gLastMeterStatsMs = 0;
static uint16_t gStandbyStep  = 0;

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
static inline void bufPixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b) {
  if (x >= kCols || y >= kRows) return;
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

static void drawStandby(uint16_t step) {
  bufClear();
  const uint8_t x = (uint8_t)(step % kCols);
  for (uint8_t y = 0; y < kRows; ++y) bufPixel(x, y, 30, 60, 90);
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
    // 超时 → 缓落回 0（不瞬跳，和真表断信号一致）
    if (!gMeterTimedOut) {
      gMeterTimedOut = true;
      Serial.println(F("[Meter] 超时 → 缓落回 0"));
    }
    for (uint8_t i = 0; i < kMeterCount; ++i) gMeters[i].target = 0.0f;
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
//  UDP
// -------------------------------------------------------------
static void handlePacket(const uint8_t* p, int len) {
  if (len < kHeaderBytes) return;
  if (p[0] != kMagic0 || p[1] != kMagic1) return;
  if (p[2] != kProtoVersion) return;

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

    gLastMeterPacketMs = millis();
    if (gMeterTimedOut) Serial.println(F("[Meter] 恢复接收"));
    gMeterTimedOut = false;
    if (!gMeterLinked) {
      gMeterLinked = true;
      Serial.println(F("[Meter] 已接管表头（演示图案停止）"));
    }

    // 收包指示：低电平点亮 30 ms
    digitalWrite(LED_BUILTIN, LOW);
    gLedOffAtMs = millis() + 30;
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
    gDirty  = true;
    gLinked = true;
    return;
  }

  // type=3 授时留给阶段 3
}

// latest-wins 的关键：必须把 socket 读空。
static void pumpUdp() {
  int sz;
  while ((sz = gUdp.parsePacket()) > 0) {
    if (sz > (int)sizeof(gRxBuf)) {
      gUdp.flush();
      continue;
    }
    const int n = gUdp.read(gRxBuf, sz);
    if (n > 0) handlePacket(gRxBuf, n);
  }
}

// -------------------------------------------------------------
//  WiFi
// -------------------------------------------------------------
static void connectWifi() {
  Serial.print(F("[WiFi] 连接 "));
  Serial.print(WIFI_SSID);
  Serial.print(F(" ... "));

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);

#if USE_STATIC_IP
  WiFi.config(MODULE_IP, GATEWAY_IP, SUBNET_MASK, DNS_IP);
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
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
  } else {
    Serial.println(F("[WiFi] 连接失败，将每 5s 重试"));
  }
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

static void printStats(uint32_t elapsedMs) {
  Serial.print(F("["));
  Serial.print(gLinked ? F("LIVE") : F("WAIT"));
  Serial.print(F("] fps="));
  Serial.print((float)gFrames * 1000.0f / (float)elapsedMs, 1);
  Serial.print(F("  pkt="));
  Serial.print(gPackets);
  Serial.print(F("  loss="));
  const uint32_t totalPkts = gPackets + gLost;
  Serial.print(totalPkts ? (float)gLost * 100.0f / (float)totalPkts : 0.0f, 1);
  Serial.print(F("%  brt="));
  Serial.print(gBrightness);
  Serial.print(F("  mA="));
  Serial.print(gLastMilliAmp);
  Serial.print(F("  cap="));
  Serial.print(gCapped);
  Serial.print(F("  rssi="));
  Serial.print(WiFi.RSSI());
  Serial.print(F("  heap="));
  Serial.println(ESP.getFreeHeap());

  gPackets = 0;
  gLost    = 0;
  gFrames  = 0;
  gCapped  = 0;
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
  Serial.println(F("=== 阶段2 · UDP 收包 + 灯阵渲染 + 5 路表头 PWM ==="));
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

  strip.Begin();
  strip.ClearTo(RgbColor(0, 0, 0));
  strip.Show();

  initMeters();
  runNeedleSweep();

  connectWifi();

  if (gUdp.begin(kUdpPort)) {
    Serial.print(F("[UDP] 监听端口 "));
    Serial.println(kUdpPort);
  } else {
    Serial.println(F("[UDP] 监听失败"));
  }

  Serial.println(F("等待上位机...（未收到表头包前跑三角波演示图案）"));

  const uint32_t now = millis();
  gLastStatsMs      = now;
  gLastStandbyMs    = now;
  gLastMeterTickMs  = now;
  gLastMeterStatsMs = now;
  gLastMeterPacketMs = now;
}

void loop() {
  pumpUdp();
  ensureWifi();

  const uint32_t now = millis();

  // ---- 表头：100 Hz 缓动渲染 ----
  tickMeters(now);

  // ---- 收包指示 LED ----
  if (gLedOffAtMs && (int32_t)(now - gLedOffAtMs) >= 0) {
    digitalWrite(LED_BUILTIN, HIGH);
    gLedOffAtMs = 0;
  }

  // ---- 灯阵 ----
  if (gLinked) {
    if (gDirty && strip.CanShow()) {
      pushFrame(gBrightness);
      ++gFrames;
      gDirty = false;
    }
  } else {
    if (now - gLastStandbyMs >= kStandbyStepMs) {
      gLastStandbyMs = now;
      drawStandby(gStandbyStep++);
      pushFrame(kStandbyBrightness);
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
