#pragma once

// =============================================================
//  配网门户（Captive Portal）
//
//  设计文档见 ../docs/08-wifi-provisioning.md
//
//  手机直连开发板的热点 → 打开任意网页 → 被 DNS 劫持到配置页 →
//  填 SSID / 密码 → 写 EEPROM → 重启 → 正常联网。
//
//  用 core 自带的 ESP8266WebServer + DNSServer，**不引第三方 WiFiManager**：
//    · 本项目一直坚持零第三方依赖（mDNS 那次吃过亏，多一个库多一层不确定性）；
//    · WiFiManager 会把 WiFi.begin / 参数存储 / 超时策略全都接管，
//      与本固件已有的重连逻辑、mDNS 注册顺序打架，排查成本高；
//    · 我们要的只是「一个表单 + 存两个字符串」，自己写 300 行更可控。
// =============================================================

#include <Arduino.h>
#include <IPAddress.h>

// 启动门户：开 AP、起 DNS 通配、监听 80 端口。返回是否成功。
// apSsid 会被复制一份内部保存（调用方可以在函数返回后释放）。
bool portalStart(const char* apSsid);

// 每个 loop 调用一次：处理 DNS 劫持请求 + HTTP 请求
void portalLoop();

// 关闭门户：停 HTTP、停 DNS、关 AP。回到纯 STA 前调用。
void portalStop();

bool portalActive();

// 供串口打印用
const char* portalApSsid();
IPAddress  portalApIp();
