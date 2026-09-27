// =============================================================
//  WiFi 配置的持久化（EEPROM）—— 实现
//
//  ★ 两个容易踩的点，都写在这里而不是口头约定：
//
//  1. 结构体填充字节（padding）
//     NetConfig 里有 uint8_t 数组和 uint32_t，编译器会在字段之间插填充。
//     填充字节的内容是**未定义的**（栈上的随机值 / flash 里的旧值）。
//     如果 checksum 把它们也算进去，就会出现「写进去的和读出来的不一致」。
//     解决办法：save 之前先 memset 整个结构体为 0，保证填充分节稳定为 0；
//     checksum 则按 offsetof 之前的全部字节计算（含这些已清零的填充），
//     读写两端用的是同一份内存布局，所以永远一致。
//
//  2. EEPROM.begin() 必须在读之前、commit() 必须在写之后
//     ESP8266 的 EEPROM 是把最后一个 flash sector 当作 RAM 缓存用的：
//     begin() 把 sector 读进缓存，put/get 只改缓存，commit() 才真正擦写 flash。
//     忘了 commit() 的现象是「重启后配置没了」，而且编译和运行时都不报错。
// =============================================================

#include "netconfig.h"
#include <EEPROM.h>
#include <string.h>

static const uint16_t kEepromAddr = 0;

static uint8_t calcChecksum(const NetConfig& c) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
  uint8_t x = 0x5A;                       // 非零初值：全 0xFF 的空白 flash 不会误判通过
  for (size_t i = 0; i < offsetof(NetConfig, checksum); ++i) x ^= p[i];
  return x;
}

bool netConfigValid(const NetConfig& c) {
  if (c.magic != kNetConfigMagic) return false;
  if (c.version != kNetConfigVersion) return false;
  if (c.checksum != calcChecksum(c)) return false;
  // SSID 非空才算配置：空 SSID 是「没配过」而不是「配了一个叫空字符串的网络」
  if (c.ssid[0] == '\0') return false;
  return true;
}

void netConfigReset(NetConfig& c) {
  memset(&c, 0, sizeof(c));
  c.magic   = kNetConfigMagic;
  c.version = kNetConfigVersion;
  c.flags   = 0;
}

bool netConfigLoad(NetConfig& out) {
  memset(&out, 0, sizeof(out));

  EEPROM.begin(kNetEepromBytes);
  EEPROM.get(kEepromAddr, out);
  // 读完就可以释放这块 RAM 缓存；ESP8266 的 EEPROM.end() 只是提交并释放缓存，
  // 这里没写过东西，直接 end() 是安全的。
  EEPROM.end();

  if (!netConfigValid(out)) {
    memset(&out, 0, sizeof(out));
    return false;
  }
  // 防御：万一 flash 里的内容没有正确结尾，强行补 '\0'，避免后面 strlen 越界
  out.ssid[kNetSsidMax] = '\0';
  out.pass[kNetPassMax] = '\0';
  return true;
}

bool netConfigSave(const NetConfig& in) {
  NetConfig c;
  memset(&c, 0, sizeof(c));
  // 只拷贝有效字段，剩下的填充字节保持 0 —— 见文件头注释第 1 条
  c.magic   = kNetConfigMagic;
  c.version = kNetConfigVersion;
  c.flags   = in.flags;
  memcpy(c.ssid, in.ssid, sizeof(c.ssid));
  memcpy(c.pass, in.pass, sizeof(c.pass));
  memcpy(c.ip,   in.ip,   sizeof(c.ip));
  memcpy(c.gw,   in.gw,   sizeof(c.gw));
  memcpy(c.mask, in.mask, sizeof(c.mask));
  memcpy(c.dns,  in.dns,  sizeof(c.dns));
  c.ssid[kNetSsidMax] = '\0';
  c.pass[kNetPassMax] = '\0';
  c.checksum = calcChecksum(c);

  EEPROM.begin(kNetEepromBytes);
  EEPROM.put(kEepromAddr, c);
  const bool ok = EEPROM.commit();
  EEPROM.end();

  if (!ok) return false;

  // 回读校验：flash 写失败（电压跌落 / sector 坏）在 commit() 上不一定报错，
  // 但回读必然对不上。这一步能立刻发现问题，而不是等下次上电才发现。
  NetConfig back;
  return netConfigLoad(back) && memcmp(&back, &c, sizeof(c)) == 0;
}

void netConfigClear() {
  EEPROM.begin(kNetEepromBytes);
  // 把 magic 所在的头 4 字节写成 0，load 时 magic 对不上即视为无配置。
  // 不用全扇区擦除：省一次擦写，效果完全一样。
  for (uint16_t i = 0; i < 4; ++i) EEPROM.write(kEepromAddr + i, 0);
  EEPROM.commit();
  EEPROM.end();
}
