#pragma once

// =============================================================
//  WiFi 配置的持久化（EEPROM）
//
//  设计文档见 ../docs/08-wifi-provisioning.md
//
//  为什么不用 SPIFFS / LittleFS：
//    · 只存 100 来字节，为它挂一个文件系统要额外占几 KB flash 和 RAM；
//    · EEPROM 库是 core 自带的，零第三方依赖（mDNS 那次就靠着这条少踩坑）；
//    · 写一次就不再动，flash 磨损可以完全不管。
//
//  ★ ESP8266 的 EEPROM 是 flash 模拟的，不是真 EEPROM：
//    读之前必须 begin()，写之后必须 commit()，否则掉电即丢。
// =============================================================

#include <Arduino.h>
#include <stddef.h>

// magic 取 "DBC1"（Dashboard Config v1）的 ASCII 倒序存放，字节序无所谓，
// 只要读写两端一致即可。用它区分「从没写过的 flash（全 0xFF）」和「真的有配置」。
static const uint32_t kNetConfigMagic   = 0x31434244u;
static const uint8_t  kNetConfigVersion = 1;

static const uint8_t  kNetSsidMax = 32;   // 802.11 规定 SSID 最长 32 字节
static const uint8_t  kNetPassMax = 64;   // WPA2-PSK 最长 63，留 1 字节给 '\0'
static const uint16_t kNetEepromBytes = 512;

// flags 位定义
static const uint8_t  kNetFlagStaticIp = 0x01;

struct NetConfig {
  uint32_t magic;
  uint8_t  version;
  uint8_t  flags;                 // bit0 = 使用静态 IP
  char     ssid[kNetSsidMax + 1];
  char     pass[kNetPassMax + 1];
  uint8_t  ip[4];
  uint8_t  gw[4];
  uint8_t  mask[4];
  uint8_t  dns[4];
  uint8_t  checksum;              // 前面所有字节的异或（含结构体填充，见 .cpp 注释）
};

// 校验 magic / version / checksum，三者全对才算一份可用配置
bool netConfigValid(const NetConfig& c);

// 从 EEPROM 读。返回 true 表示读出了一份**通过校验**的配置。
// 失败时 out 会被清零，调用方可以安全地使用默认值。
bool netConfigLoad(NetConfig& out);

// 写入 EEPROM（内部会先算 checksum 再 commit）
bool netConfigSave(const NetConfig& c);

// 清除：把 magic 写坏，下次 load 必然失败
void netConfigClear();

// 把 c 填成一份「空但结构合法」的配置，便于调用方只改关心的字段
void netConfigReset(NetConfig& c);
