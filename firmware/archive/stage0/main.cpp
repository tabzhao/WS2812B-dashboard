// =============================================================
//  【归档】阶段 0 · 仅验证灯阵映射的最小固件
//
//  状态：已于 2026-09-17 在真实硬件上验证通过，映射公式确认无误。
//  用途：将来灯阵出现"画面错乱"类故障时，把它复制回 src/main.cpp
//        重新烧录，用 8 个模式快速判断是映射问题还是硬件问题。
//
//  用法：cp archive/stage0/main.cpp src/main.cpp  然后 pio run -t upload
//        （记得先断开灯阵 5V，否则上电冲击会让芯片进不了下载模式）
//
//  设计依据：docs/03-matrix-mapping.md
//  验证步骤：见本文件末尾的八个模式判据
// =============================================================

#include <Arduino.h>
#include <NeoPixelBus.h>
#include "config.h"

// -------------------------------------------------------------
//  灯总线
//  I2S DMA 方式固定占用 GPIO3 (RXD0)，构造时的 pin 参数会被忽略。
// -------------------------------------------------------------
NeoPixelBus<NeoGrbFeature, NeoEsp8266Dma800KbpsMethod> strip(kPixelCount);

// -------------------------------------------------------------
//  XY 行序帧缓冲
//  刻意做成"未来 UDP 载荷"的形态（行优先、左上角原点、每像素 3 字节 RGB），
//  所以这个固件同时也是后续渲染管线（XY 缓冲 → 功耗封顶 → 重映射 → DMA）的一次演练。
// -------------------------------------------------------------
static uint8_t gFrameXY[kPixelCount * 3];

// -------------------------------------------------------------
//  像素索引映射  ★不要自行推导★
//
//  来自已跑通硬件的参考工程（本地另一个 ESP-IDF 工程，未包含在本仓库）
//    solo_idf_arduino/main/effects.cpp
//  对应 config.h 的  TILE_ROW_SERPENTINE = false / TILE_ROTATE_LEFT_90 = true
//
//    index = (x/8)*64 + (x%8)*8 + (7-y)
// -------------------------------------------------------------
static inline uint16_t xyToIndex(uint8_t x, uint8_t y) {
  return ((uint16_t)(x >> 3) << 6) + ((uint16_t)(x & 0x07) << 3) + (7 - y);
}

// 逆映射：由硬件串号反推屏幕坐标（扫描模式用）
static inline void indexToXY(uint16_t i, uint8_t& x, uint8_t& y) {
  x = (uint8_t)(((i >> 6) << 3) + ((i % 64) >> 3));
  y = (uint8_t)(7 - (i % 8));
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

// 施加全局亮度 + 软件功耗封顶，然后按映射推给 DMA
static void pushFrame(uint8_t brightness) {
  const uint16_t n = kPixelCount * 3;

  // 1) 全局亮度
  uint32_t total = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const uint16_t v = (uint16_t)(((uint16_t)gFrameXY[i] * brightness) / 255);
    gFrameXY[i] = (uint8_t)v;
    total += v;
  }

  // 2) 功耗封顶：I(mA) = 0.0784 × Σ(R+G+B)
  if (total > kMaxChannelSum) {
    const uint16_t scale = (uint16_t)(((uint32_t)kMaxChannelSum << 8) / total);
    for (uint16_t i = 0; i < n; ++i) {
      gFrameXY[i] = (uint8_t)(((uint16_t)gFrameXY[i] * scale) >> 8);
    }
    Serial.print(F("        [功耗封顶触发] scale="));
    Serial.println(scale);
  }

  // 3) XY 行序 → 硬件串号 → DMA
  for (uint8_t y = 0; y < kRows; ++y) {
    for (uint8_t x = 0; x < kCols; ++x) {
      const uint16_t i = (uint16_t)(((uint16_t)y * kCols + x) * 3);
      strip.SetPixelColor(
          xyToIndex(x, y),
          RgbColor(gFrameXY[i], gFrameXY[i + 1], gFrameXY[i + 2]));
    }
  }
  strip.Show();
}

// -------------------------------------------------------------
//  演示模式
// -------------------------------------------------------------
enum Mode : uint8_t {
  kModeRawSweep  = 0,   // 按硬件串号扫描
  kModeXySweep,         // 按逻辑 XY 行序扫描（决定性测试）
  kModeColumnBar,       // 整列竖条，从左往右
  kModeRowBar,          // 整行横条，从上往下
  kModeBorder,          // 矩形边框
  kModeTileColor,       // 四块不同颜色（验证块序）
  kModeGradient,        // 横向彩虹渐变（连续性 + 触发功耗封顶）
  kModeCorners,         // 关键点静态标记（方便拍照核对）
  kModeCount
};

struct ModeDef {
  const char* name;
  const char* hint;
  uint16_t    stepMs;
  uint16_t    steps;
};

static const ModeDef kModeDefs[kModeCount] = {
  {"1/8 串号扫描 (白)",
   "白点应从【左下角】起步，沿列【向上】爬，到底后右移一列再从下往上；"
   "走完 8 列进入右侧下一块",
   40,  256},
  {"2/8 XY 行序扫描 (洋红)",
   "洋红点应从【左上角】起步，【从左到右】走完一行后下移一行；"
   "若看到竖向移动说明旋转写反了",
   40,  256},
  {"3/8 竖条扫描 (青)",
   "整列青色竖条应【从左向右】平移",
   120, 32},
  {"4/8 横条扫描 (橙)",
   "整行橙色横条应【从上向下】平移",
   400, 8},
  {"5/8 矩形边框 (红)",
   "应看到一个干净的 32x8 红色矩形轮廓，四边等宽",
   500, 12},
  {"6/8 分块配色",
   "从左到右依次应为【红 / 绿 / 蓝 / 黄】，每块 8 列宽",
   500, 12},
  {"7/8 横向彩虹渐变",
   "整屏应为连续光谱，沿 X 轴平滑过渡并整体缓慢滚动；"
   "此模式会触发功耗封顶，观察是否被压暗",
   40,  150},
  {"8/8 关键点标记 (静态)",
   "背景微弱；左上角红、右上角绿、左下角蓝、右下角黄、各块起点洋红",
   500, 24},
};

// HSV → RGB，s 固定为 255（h: 0..255 色相轮，v: 0..255 明度）
static void hsvToRgb(uint8_t h, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  const uint8_t region = h / 43;
  const uint8_t rem    = (uint8_t)((h - region * 43) * 6);
  const uint8_t p      = 0;
  const uint8_t q      = (uint8_t)(((uint16_t)v * (255 - rem)) >> 8);
  const uint8_t t      = (uint8_t)(((uint16_t)v * rem) >> 8);
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

static void drawRawSweep(uint16_t step) {
  for (uint8_t k = 0; k < 5; ++k) {          // 4 级拖尾
    const int16_t i = (int16_t)step - k;
    if (i < 0) continue;
    uint8_t x, y;
    indexToXY((uint16_t)i, x, y);
    const uint8_t v = (uint8_t)(255 - k * 45);
    bufPixel(x, y, v, v, v);
  }
}

static void drawXySweep(uint16_t step) {
  for (uint8_t k = 0; k < 5; ++k) {
    const int16_t i = (int16_t)step - k;
    if (i < 0) continue;
    const uint8_t x = (uint8_t)(i % kCols);
    const uint8_t y = (uint8_t)(i / kCols);
    const uint8_t v = (uint8_t)(255 - k * 45);
    bufPixel(x, y, v, 0, v);
  }
}

static void drawColumnBar(uint16_t step) {
  const uint8_t x = (uint8_t)(step % kCols);
  for (uint8_t y = 0; y < kRows; ++y) bufPixel(x, y, 0, 200, 255);
}

static void drawRowBar(uint16_t step) {
  const uint8_t y = (uint8_t)(step % kRows);
  for (uint8_t x = 0; x < kCols; ++x) bufPixel(x, y, 255, 128, 0);
}

static void drawBorder() {
  for (uint8_t x = 0; x < kCols; ++x) {
    bufPixel(x, 0, 200, 0, 0);
    bufPixel(x, kRows - 1, 200, 0, 0);
  }
  for (uint8_t y = 0; y < kRows; ++y) {
    bufPixel(0, y, 200, 0, 0);
    bufPixel(kCols - 1, y, 200, 0, 0);
  }
}

static void drawTileColor() {
  static const uint8_t kTileRGB[4][3] = {
    {128, 0, 0}, {0, 128, 0}, {0, 0, 128}, {128, 128, 0}
  };
  for (uint8_t x = 0; x < kCols; ++x) {
    const uint8_t t = (uint8_t)(x >> 3);
    for (uint8_t y = 0; y < kRows; ++y) {
      bufPixel(x, y, kTileRGB[t][0], kTileRGB[t][1], kTileRGB[t][2]);
    }
  }
}

static void drawGradient(uint16_t step) {
  const uint8_t base = (uint8_t)(step * 2);
  for (uint8_t x = 0; x < kCols; ++x) {
    uint8_t r, g, b;
    hsvToRgb((uint8_t)(base + x * 8), 255, r, g, b);
    for (uint8_t y = 0; y < kRows; ++y) bufPixel(x, y, r, g, b);
  }
}

static void drawCorners() {
  for (uint8_t x = 0; x < kCols; ++x) {
    for (uint8_t y = 0; y < kRows; ++y) bufPixel(x, y, 6, 6, 6);
  }
  // 各块起点（该块串号最小的那颗，位于屏幕底行）
  for (uint8_t t = 0; t < 4; ++t) bufPixel((uint8_t)(t * 8), 7, 255, 0, 255);
  // 四角（最后画，覆盖上面的洋红）
  bufPixel(0, 0, 255, 0, 0);              // 左上角 = 串号 7
  bufPixel(kCols - 1, 0, 0, 255, 0);      // 右上角 = 串号 255
  bufPixel(0, kRows - 1, 0, 0, 255);      // 左下角 = 串号 0
  bufPixel(kCols - 1, kRows - 1, 255, 255, 0);  // 右下角 = 串号 248
}

static void drawMode(uint8_t mode, uint16_t step) {
  switch (mode) {
    case kModeRawSweep:  drawRawSweep(step);  break;
    case kModeXySweep:   drawXySweep(step);   break;
    case kModeColumnBar: drawColumnBar(step); break;
    case kModeRowBar:    drawRowBar(step);    break;
    case kModeBorder:    drawBorder();        break;
    case kModeTileColor: drawTileColor();     break;
    case kModeGradient:  drawGradient(step);  break;
    case kModeCorners:   drawCorners();       break;
    default: break;
  }
}

// -------------------------------------------------------------
//  状态
// -------------------------------------------------------------
static uint8_t  gMode            = kModeRawSweep;
static uint16_t gStep            = 0;
static uint32_t gLastStepMs      = 0;
static uint8_t  gBrightnessIndex = kDefaultBrightnessIndex;

static void enterMode(uint8_t mode) {
  gMode   = mode;
  gStep   = 0;
  gLastStepMs = millis();
  Serial.println();
  Serial.print(F("[MODE "));
  Serial.print(kModeDefs[mode].name);
  Serial.println(F("]"));
  Serial.print(F("       期望："));
  Serial.println(kModeDefs[mode].hint);
}

static void setBrightnessIndex(uint8_t idx) {
  gBrightnessIndex = idx % kBrightnessCount;
  Serial.print(F("[BRIGHTNESS] V="));
  Serial.println(kBrightnessSteps[gBrightnessIndex]);
}

static void handleButton() {
  static bool     lastDown = false;
  static uint32_t downAt   = 0;

  const bool down = (digitalRead(kButtonPin) == LOW);   // 板载 FLASH 键：按下为低

  if (down && !lastDown) {
    downAt = millis();
  } else if (!down && lastDown) {
    const uint32_t held = millis() - downAt;
    if (held >= kLongPressMs) {
      setBrightnessIndex(gBrightnessIndex + 1);
    } else if (held >= 30) {            // 去抖
      enterMode((gMode + 1) % kModeCount);
    }
  }
  lastDown = down;
}

// -------------------------------------------------------------
//  setup / loop
// -------------------------------------------------------------
static void printExpectedTable() {
  Serial.println(F("--- 关键串号 → 屏幕坐标（应与 docs/03 对照表一致）---"));
  const uint16_t keys[] = {0, 7, 63, 64, 127, 128, 191, 192, 248, 255};
  for (uint8_t n = 0; n < sizeof(keys) / sizeof(keys[0]); ++n) {
    uint8_t x, y;
    indexToXY(keys[n], x, y);
    Serial.print(F("    串号 "));
    Serial.print(keys[n]);
    Serial.print(F("  ->  ("));
    Serial.print(x);
    Serial.print(F(", "));
    Serial.print(y);
    Serial.println(F(")"));
  }
}

void setup() {
  // ★ 顺序至关重要：必须先 Serial.begin()，再 strip.Begin()。
  //   否则 Serial 会把 GPIO3 抢回做 RX，DMA 输出完全不生效（现象：灯全不亮）。
  Serial.begin(kSerialBaud);
  Serial.println();
  Serial.println(F("=== 阶段0 · 灯阵映射验证固件 ==="));
  Serial.println(F("映射公式 index = (x/8)*64 + (x%8)*8 + (7-y)"));
  Serial.println();
  printExpectedTable();
  Serial.println();
  Serial.println(F("操作：短按板载 FLASH 键 = 下一个模式；长按(>0.7s) = 切换亮度"));
  Serial.println(F("注意：GPIO3 的 RX 被 DMA 占用，串口只能输出、收不了命令"));

  strip.Begin();
  strip.ClearTo(RgbColor(0, 0, 0));
  strip.Show();

  pinMode(kButtonPin, INPUT_PULLUP);
  setBrightnessIndex(gBrightnessIndex);
  enterMode(kModeRawSweep);
}

void loop() {
  handleButton();

  const uint32_t now = millis();
  const ModeDef& def = kModeDefs[gMode];

  if (now - gLastStepMs >= def.stepMs) {
    gLastStepMs = now;

    bufClear();
    drawMode(gMode, gStep);
    pushFrame(kBrightnessSteps[gBrightnessIndex]);

    if (++gStep >= def.steps) {
      enterMode((gMode + 1) % kModeCount);
    }
  }

  yield();
}
