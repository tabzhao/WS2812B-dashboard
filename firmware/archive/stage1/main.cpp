// =============================================================
//  阶段 1 · UDP 收包 + 灯阵渲染（最小可验证版）
//
//  本阶段只做三件事：连 WiFi、收 type=2 像素包、推给灯阵。
//  不含 OTA、不含超时降级、不含表头、不含时钟 —— 那些在后续阶段叠加。
//
//  映射公式已由阶段 0 实测确认，见 docs/03-matrix-mapping.md
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

// XY 行序帧缓冲：与 UDP 载荷同构（行优先、左上角原点、每像素 3 字节 RGB）
static uint8_t gFrameXY[kPixelCount * 3];
static uint8_t gRxBuf[kRxBufferBytes];

static WiFiUDP gUdp;

// -------------------------------------------------------------
//  运行时状态
// -------------------------------------------------------------
static uint8_t  gBrightness = 32;
static bool     gDirty      = false;   // 收到新帧但未推
static bool     gLinked     = false;   // 是否已收到过像素帧

static uint32_t gPackets    = 0;
static uint32_t gLost       = 0;
static uint32_t gFrames     = 0;
static uint32_t gCapped     = 0;
static uint16_t gLastSeq    = 0;
static bool     gSeqValid   = false;
static uint16_t gLastMilliAmp = 0;

static uint32_t gLastStatsMs  = 0;
static uint32_t gLastStandbyMs = 0;
static uint32_t gLastWifiMs   = 0;
static uint16_t gStandbyStep  = 0;

// -------------------------------------------------------------
//  像素索引映射  ★不要自行推导★
//    index = (x/8)*64 + (x%8)*8 + (7-y)
//  来自已跑通硬件的参考工程，并由阶段 0 逐点验证。
// -------------------------------------------------------------
static inline uint16_t xyToIndex(uint8_t x, uint8_t y) {
  return ((uint16_t)(x >> 3) << 6) + ((uint16_t)(x & 0x07) << 3) + (7 - y);
}

// -------------------------------------------------------------
//  帧缓冲操作
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

// 把 XY 行序缓冲经「亮度 × 功耗封顶」后推给 DMA。
//
// ★ 关键：本函数不得修改 gFrameXY。
//   同一帧可能被重复推（上位机发得慢 / 待机图案），若就地乘亮度，
//   画面会一次比一次暗，且极难排查。
static void pushFrame(uint8_t brightness) {
  const uint16_t n = kPixelCount * 3;

  // 1) 亮度后的通道总和，用于功耗封顶
  uint32_t sum = 0;
  for (uint16_t i = 0; i < n; ++i) sum += gFrameXY[i];
  sum = (uint32_t)((uint64_t)sum * brightness / 255);

  // 2) 功耗封顶：I(mA) = 0.0784 × Σ(R+G+B)，上限 kMaxChannelSum
  uint16_t capScale = 256;            // 8.8 定点，256 = 1.0
  if (sum > kMaxChannelSum) {
    capScale = (uint16_t)(((uint32_t)kMaxChannelSum << 8) / sum);
    if (capScale < 1) capScale = 1;
    ++gCapped;
  }
  const uint32_t actualSum = (sum * capScale) >> 8;
  gLastMilliAmp = (uint16_t)(actualSum * 784 / 10000);   // ×0.0784

  // 3) 亮度与封顶合并成单个 8.8 系数，每通道只做一次乘法
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
//  待机图案：未收到任何像素帧时显示，一条暗竖条缓慢右移
// -------------------------------------------------------------
static void drawStandby(uint16_t step) {
  bufClear();
  const uint8_t x = (uint8_t)(step % kCols);
  for (uint8_t y = 0; y < kRows; ++y) bufPixel(x, y, 30, 60, 90);
}

// -------------------------------------------------------------
//  UDP
// -------------------------------------------------------------
static void handlePacket(const uint8_t* p, int len) {
  if (len < kHeaderBytes) return;
  if (p[0] != kMagic0 || p[1] != kMagic1) return;      // 非本协议的包，直接丢
  if (p[2] != kProtoVersion) return;

  const uint8_t  type = p[3];
  const uint16_t seq  = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));   // 小端

  if (type != kTypePixels) return;                     // 阶段 1 只处理像素帧
  if (len < kPixelsPacketLen) return;

  ++gPackets;
  if (gSeqValid) {
    const uint16_t gap = (uint16_t)(seq - gLastSeq);   // 无符号回绕天然正确
    if (gap > 1) gLost += (uint32_t)(gap - 1);
  }
  gLastSeq  = seq;
  gSeqValid = true;

  memcpy(gFrameXY, p + kHeaderBytes, kPixelPayloadBytes);

  // 固件侧亮度硬上限，电源的安全网
  gBrightness = (p[6] > kMaxBrightness) ? kMaxBrightness : p[6];
  gDirty  = true;
  gLinked = true;
}

// latest-wins 的关键：必须把 socket 读空。
// 每 loop 只读一个包的话，上位机发得比 loop 快时缓冲会堆积，
// 延迟线性增长 —— 那是"队列化"的隐形版本。
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
  // 必须关 modem sleep：默认开启会在收包路径上插入 100~300ms 延迟尖峰
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
  Serial.println(F("=== 阶段1 · UDP 收包 + 灯阵渲染 ==="));
  Serial.print(F("映射 index = (x/8)*64 + (x%8)*8 + (7-y)   监听 UDP "));
  Serial.println(kUdpPort);

  strip.Begin();
  strip.ClearTo(RgbColor(0, 0, 0));
  strip.Show();

  connectWifi();

  if (gUdp.begin(kUdpPort)) {
    Serial.print(F("[UDP] 监听端口 "));
    Serial.println(kUdpPort);
  } else {
    Serial.println(F("[UDP] 监听失败"));
  }

  gLastStatsMs   = millis();
  gLastStandbyMs = millis();
  Serial.println(F("等待上位机像素帧...（未收到时显示暗竖条待机图案）"));
}

void loop() {
  pumpUdp();
  ensureWifi();

  const uint32_t now = millis();

  if (gLinked) {
    // 只有收到新帧才推，且必须查 CanShow()（否则 Show() 会阻塞等 DMA）
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

  if (now - gLastStatsMs >= kStatsIntervalMs) {
    printStats(now - gLastStatsMs);
    gLastStatsMs = now;
  }

  yield();
}
