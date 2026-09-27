#pragma once

// =============================================================
//  WiFi 与网络配置模板
//
//  用法：把本文件复制为同目录下的 secrets.h，填入你的实际配置。
//  secrets.h 已被 .gitignore 忽略，不会误提交。
//  若不存在 secrets.h，固件会自动回退到本模板（连不上网，但能编译）。
// =============================================================

// ⚠️ ESP8266 只支持 2.4 GHz，5 GHz 的 SSID 扫描不到。
#define WIFI_SSID       "YOUR_2.4G_SSID"
#define WIFI_PASSWORD   "YOUR_PASSWORD"

// ---- 寻址方式 ----
// 1 = 静态 IP（保留作兜底），0 = DHCP（推荐）
//
// 【为什么推荐 DHCP】
// 固件注册了 mDNS 服务 `_dashboard._udp`，上位机靠 browse 自动发现，
// 不用知道 IP，所以换网段、路由器改 DHCP 池都不需要改这里重新烧录。
// 只有当你确认网络封了多播（AP 隔离 / 企业 WiFi / VLAN）时才改回 1。
#define USE_STATIC_IP   0

// 下面的静态地址只在 USE_STATIC_IP=1 时生效。
// 注意避开路由器 DHCP 池，否则会间歇断线。

// 下面的地址按你的实际网段改。用电脑执行 `ipconfig getifaddr en0`(macOS)
// 或 `ip a`(Linux) 看本机 IP，网关通常是路由器地址（如 192.168.1.1）。
#define MODULE_IP       IPAddress(192, 168, 1, 50)
#define GATEWAY_IP      IPAddress(192, 168, 1, 1)
#define SUBNET_MASK     IPAddress(255, 255, 255, 0)
#define DNS_IP          IPAddress(192, 168, 1, 1)
