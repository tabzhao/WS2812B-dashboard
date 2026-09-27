// =============================================================
//  配网门户（Captive Portal）—— 实现
//
//  ★ 三个不做就不好用的细节，都在这里处理掉了：
//
//  1. DNS 通配 + 404 重定向，才叫 Captive Portal
//     只起一个 WebServer 的话，手机连上热点后不会有任何提示，
//     用户得自己猜到要输入 192.168.4.1。
//     DNSServer 把 * 全部解析到本机，配合 404 → 302 到 /，
//     Android 会弹「此网络需要登录」，iOS 会弹出登录页 —— 这才是"直连就能配"。
//
//  2. 必须 WIFI_AP_STA，不能只 WIFI_AP
//     只有同时保留 STA 才能扫描周边 WiFi 列表。纯 AP 模式下
//     scanNetworks() 直接失败，页面上的下拉框会是空的。
//
//  3. 扫描结果要缓存
//     scanNetworks() 同步阻塞 1~3 秒，这期间主循环停摆、灯阵卡住。
//     所以只在门户启动和点「重新扫描」时扫，结果存起来给页面用。
// =============================================================

#include "portal.h"
#include "config.h"
#include "netconfig.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <string.h>

// encryptionType() 的返回值。不用 ENC_TYPE_* 宏是因为各版本 core 名字略有出入，
// 自己定义成数值最稳。
static const uint8_t kEncNone = 0;
static const uint8_t kEncWep  = 1;
static const uint8_t kEncTkip = 2;
static const uint8_t kEncCcmp = 4;
static const uint8_t kEncAuto = 6;

static ESP8266WebServer gServer(kPortalHttpPort);
static DNSServer        gDns;
static bool             gActive = false;
static char             gApSsid[32] = {0};
static bool             gScanDone   = false;

struct ApItem {
  char    ssid[kNetSsidMax + 1];
  int8_t  rssi;
  uint8_t enc;
};
static ApItem gAps[kPortalMaxAps];
static int    gApCount = 0;

// -------------------------------------------------------------
//  扫描
// -------------------------------------------------------------
static void scanAps() {
  gApCount = 0;
  WiFi.scanDelete();

  // 第二个参数 show_hidden=true：隐藏 SSID 的网络也列出来，
  // 用户可以手动输入名字去连。
  const int n = WiFi.scanNetworks(false, true);
  if (n > 0) {
    for (int i = 0; i < n && gApCount < (int)kPortalMaxAps; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0 || ssid.length() > kNetSsidMax) continue;

      const int8_t  rssi = (int8_t)WiFi.RSSI(i);
      const uint8_t enc  = WiFi.encryptionType(i);

      // 同一个 SSID 常常有多个 BSSID（多 AP / 中继）。去重，保留信号最强的那个，
      // 否则下拉框里会出现一串同名项，用户根本分不清该选哪个。
      int found = -1;
      for (int k = 0; k < gApCount; ++k) {
        if (strcmp(gAps[k].ssid, ssid.c_str()) == 0) { found = k; break; }
      }
      if (found >= 0) {
        if (rssi > gAps[found].rssi) {
          gAps[found].rssi = rssi;
          gAps[found].enc  = enc;
        }
        continue;
      }

      strncpy(gAps[gApCount].ssid, ssid.c_str(), kNetSsidMax);
      gAps[gApCount].ssid[kNetSsidMax] = '\0';
      gAps[gApCount].rssi = rssi;
      gAps[gApCount].enc  = enc;
      ++gApCount;
    }
  }
  WiFi.scanDelete();

  // 按信号降序冒泡（n ≤ kPortalMaxAps，冒泡足够，不值得引入 qsort）
  for (int i = 1; i < gApCount; ++i) {
    for (int j = i; j > 0 && gAps[j].rssi > gAps[j - 1].rssi; --j) {
      const ApItem t = gAps[j];
      gAps[j] = gAps[j - 1];
      gAps[j - 1] = t;
    }
  }
  gScanDone = true;
}

// -------------------------------------------------------------
//  HTML 小工具
// -------------------------------------------------------------
// SSID 可能含 & < > " '，不转义会让页面结构被破坏（尤其是双引号截断 value=）
static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;");  break;
      default:   out += s[i];        break;
    }
  }
  return out;
}

static const char* rssiLabel(int8_t r) {
  if (r >= -55) return "强";
  if (r >= -70) return "中";
  if (r >= -85) return "弱";
  return "极弱";
}

static const char* encLabel(uint8_t e) {
  switch (e) {
    case kEncNone: return "开放";
    case kEncWep:  return "WEP";
    case kEncTkip: return "WPA";
    case kEncCcmp: return "WPA2";
    case kEncAuto: return "WPA/WPA2";
    default:       return "加密";
  }
}

// 解析 "192.168.1.50" → 4 字节。空串也算通过（表示未填，交给调用方处理）。
static bool parseIp(const String& s, uint8_t out[4]) {
  if (s.length() == 0) return false;
  uint8_t part = 0;
  uint16_t v = 0;
  uint8_t digits = 0;
  for (size_t i = 0; i <= s.length(); ++i) {
    const char c = (i < s.length()) ? s[i] : '.';
    if (c >= '0' && c <= '9') {
      v = (uint16_t)(v * 10 + (c - '0'));
      if (++digits > 3 || v > 255) return false;
      continue;
    }
    if (c != '.') return false;
    if (digits == 0) return false;
    if (part >= 4) return false;
    out[part++] = (uint8_t)v;
    v = 0;
    digits = 0;
  }
  return part == 4;
}

static String ipStr(const uint8_t a[4]) {
  String s;
  s.reserve(16);
  for (uint8_t i = 0; i < 4; ++i) {
    if (i) s += '.';
    s += (unsigned)a[i];
  }
  return s;
}

// -------------------------------------------------------------
//  页面外壳
// -------------------------------------------------------------
static void pageHead(String& h, const char* title) {
  h.reserve(4096);
  h += F("<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">");
  h += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  h += F("<title>");
  h += title;
  h += F("</title><style>");
  h += F("*{box-sizing:border-box}body{margin:0;padding:18px;background:#f2f3f5;color:#1f2328;");
  h += F("font:15px/1.6 -apple-system,BlinkMacSystemFont,\"PingFang SC\",\"Helvetica Neue\",Arial,sans-serif}");
  h += F(".card{max-width:460px;margin:0 auto;background:#fff;border-radius:14px;padding:20px;");
  h += F("box-shadow:0 1px 3px rgba(0,0,0,.08)}");
  h += F("h1{margin:0 0 4px;font-size:19px;font-weight:600}");
  h += F(".sub{margin:0 0 18px;color:#656d76;font-size:13px}");
  h += F("label{display:block;margin:14px 0 6px;font-size:13px;color:#424a53}");
  h += F("input[type=text],input[type=password],select{width:100%;padding:11px 12px;font-size:16px;");
  h += F("border:1px solid #d0d7de;border-radius:9px;background:#fff;color:#1f2328}");
  h += F("select{margin-bottom:8px}");
  h += F(".row{display:flex;gap:8px}.row>*{flex:1}");
  h += F("button{width:100%;margin-top:20px;padding:13px;font-size:16px;font-weight:600;color:#fff;");
  h += F("background:#1f6feb;border:0;border-radius:9px}");
  h += F(".msg{padding:10px 12px;border-radius:9px;font-size:14px;margin-bottom:14px}");
  h += F(".err{background:#ffebe9;color:#82071e}.ok{background:#dafbe1;color:#0a3622}");
  h += F(".links{margin-top:18px;font-size:13px;color:#656d76;text-align:center}");
  h += F(".links a{color:#0969da;text-decoration:none;margin:0 6px}");
  h += F("details{margin-top:16px;font-size:13px;color:#424a53}");
  h += F("summary{cursor:pointer;padding:6px 0}");
  h += F("</style></head><body><div class=\"card\">");
  h += F("<h1>");
  h += title;
  h += F("</h1>");
}

static void pageFoot(String& h) {
  h += F("</div></body></html>");
}

// -------------------------------------------------------------
//  GET /
// -------------------------------------------------------------
static void handleRoot() {
  const bool rescan = gServer.hasArg("scan");

  // 重新扫描只在用户点链接时做，避免每次刷新页面都卡 1~3 秒
  if (rescan || !gScanDone) {
    scanAps();
  }

  String h;
  pageHead(h, "仪表盘配网");

  h += F("<p class=\"sub\">设备 <b>");
  h += htmlEscape(String(gApSsid));
  h += F("</b> · 配网地址 ");
  h += WiFi.softAPIP().toString();
  h += F("</p>");

  if (gApCount == 0) {
    h += F("<div class=\"msg err\">没扫到任何 2.4 GHz 网络。ESP8266 不支持 5 GHz，");
    h += F("请确认路由器开了 2.4G，或直接在下方手动输入 WiFi 名称。</div>");
  }

  h += F("<form method=\"post\" action=\"/save\">");

  if (gApCount > 0) {
    h += F("<label>附近的网络（点一下自动填入）</label>");
    h += F("<select onchange=\"if(this.value){document.getElementById('ssid').value=this.value}\">");
    h += F("<option value=\"\">— 请选择 —</option>");
    for (int i = 0; i < gApCount; ++i) {
      h += F("<option value=\"");
      h += htmlEscape(String(gAps[i].ssid));
      h += F("\">");
      h += htmlEscape(String(gAps[i].ssid));
      h += F("　· ");
      h += rssiLabel(gAps[i].rssi);
      h += F(" · ");
      h += encLabel(gAps[i].enc);
      h += F("</option>");
    }
    h += F("</select>");
  }

  h += F("<label>WiFi 名称（2.4 GHz）</label>");
  h += F("<input id=\"ssid\" name=\"ssid\" type=\"text\" autocomplete=\"off\" ");
  h += F("placeholder=\"例如 MyHome_2.4G\" spellcheck=\"false\">");

  h += F("<label>密码</label>");
  h += F("<input name=\"pass\" type=\"password\" autocomplete=\"new-password\" ");
  h += F("placeholder=\"开放网络留空\">");

  h += F("<details><summary>高级：使用静态 IP（一般不用）</summary>");
  h += F("<label><input type=\"checkbox\" name=\"static\" value=\"1\" ");
  h += F("onchange=\"document.getElementById('ipbox').style.display=this.checked?'block':'none'\"> ");
  h += F("启用静态 IP</label>");
  h += F("<div id=\"ipbox\" style=\"display:none\">");
  h += F("<label>模块 IP</label><input name=\"ip\" type=\"text\" placeholder=\"192.168.5.50\">");
  h += F("<label>网关</label><input name=\"gw\" type=\"text\" placeholder=\"192.168.5.1\">");
  h += F("<label>子网掩码</label><input name=\"mask\" type=\"text\" placeholder=\"255.255.255.0\">");
  h += F("<label>DNS</label><input name=\"dns\" type=\"text\" placeholder=\"192.168.5.1\">");
  h += F("</div></details>");

  h += F("<button type=\"submit\">保存并连接</button></form>");

  h += F("<p class=\"links\">");
  h += F("<a href=\"/?scan=1\">重新扫描</a>");
  h += F("<a href=\"/forget\" onclick=\"return confirm('清除已保存的 WiFi 配置并重启？')\">清除配置</a>");
  h += F("<a href=\"/reboot\">重启设备</a>");
  h += F("</p>");

  pageFoot(h);
  // 注意第二个参数不能用 F()：send() 的 content_type 形参是 const char*，
  // 传 __FlashStringHelper* 会编译不过。这两三个字符串留在 RAM 里无所谓。
  gServer.send(200, "text/html; charset=utf-8", h);
}

// -------------------------------------------------------------
//  POST /save
// -------------------------------------------------------------
static void sendResult(bool ok, const String& title, const String& body, bool reload) {
  String h;
  pageHead(h, "仪表盘配网");
  h += F("<div class=\"msg ");
  h += ok ? F("ok") : F("err");
  h += F("\"><b>");
  h += htmlEscape(title);
  h += F("</b><br>");
  h += htmlEscape(body);
  h += F("</div>");
  if (reload) {
    h += F("<p class=\"sub\">页面会在 25 秒后自动刷新。</p>");
    h += F("<meta http-equiv=\"refresh\" content=\"25;url=/\">");
  } else {
    h += F("<p class=\"links\"><a href=\"/\">返回</a></p>");
  }
  pageFoot(h);
  gServer.send(200, "text/html; charset=utf-8", h);
}

static void handleSave() {
  String ssid = gServer.arg("ssid");
  String pass = gServer.arg("pass");
  ssid.trim();
  // ★ 密码不能 trim：首尾空格是合法字符，trim 掉会连不上且极难排查。

  if (ssid.length() == 0) {
    sendResult(false, "没填 WiFi 名称", "请填写要连接的 WiFi 名称，或从上方列表中选一个。", false);
    return;
  }
  if (ssid.length() > kNetSsidMax) {
    sendResult(false, "WiFi 名称太长", "SSID 最长 32 个字符。", false);
    return;
  }
  if (pass.length() > kNetPassMax) {
    sendResult(false, "密码太长", "密码最长 64 个字符。", false);
    return;
  }

  NetConfig c;
  netConfigReset(c);
  strncpy(c.ssid, ssid.c_str(), kNetSsidMax);
  strncpy(c.pass, pass.c_str(), kNetPassMax);

  if (gServer.arg("static") == "1") {
    uint8_t ip[4], gw[4], mask[4], dns[4];
    if (!parseIp(gServer.arg("ip"), ip) || !parseIp(gServer.arg("gw"), gw)) {
      sendResult(false, "静态 IP 填写不完整", "至少要填模块 IP 和网关。", false);
      return;
    }
    if (!parseIp(gServer.arg("mask"), mask)) {
      mask[0] = 255; mask[1] = 255; mask[2] = 255; mask[3] = 0;   // 默认 /24
    }
    if (!parseIp(gServer.arg("dns"), dns)) {
      memcpy(dns, gw, 4);                                          // 默认用网关
    }
    memcpy(c.ip, ip, 4);
    memcpy(c.gw, gw, 4);
    memcpy(c.mask, mask, 4);
    memcpy(c.dns, dns, 4);
    c.flags |= kNetFlagStaticIp;
  }

  if (!netConfigSave(c)) {
    sendResult(false, "写入失败", "配置没能写进 flash（回读校验不通过）。请重试，或重启设备后再配一次。", false);
    return;
  }

  Serial.print(F("[Portal] 已保存 SSID="));
  Serial.print(c.ssid);
  if (c.flags & kNetFlagStaticIp) {
    Serial.print(F("  静态 IP="));
    Serial.println(ipStr(c.ip));
  } else {
    Serial.println(F("  (DHCP)"));
  }

  // 保存成功 → 重启。重启后走正常流程：读 EEPROM → 连 WiFi。
  // 连不上会自动回到本门户，用户刷新页面即可重试。
  sendResult(true, "已保存，正在重启",
            String("设备将尝试连接「") + ssid + "」。若十几秒后手机上又能搜到本热点，"
            "说明没连上（密码错 / 不是 2.4G / 信号太差），请重新配置。", true);

  // 让响应先发出去再重启。ESP8266WebServer 的 send 是同步写，
  // 但 TCP 缓冲未必已经推到对端，留一点时间更稳。
  delay(400);
  ESP.restart();
}

// -------------------------------------------------------------
//  /forget 与 /reboot
// -------------------------------------------------------------
static void handleForget() {
  netConfigClear();
  Serial.println(F("[Portal] 已清除 WiFi 配置，重启"));
  sendResult(true, "已清除配置", "设备重启后会自动重新进入配网模式。", true);
  delay(400);
  ESP.restart();
}

static void handleReboot() {
  sendResult(true, "正在重启", "设备将在 2 秒后重启。", true);
  delay(400);
  ESP.restart();
}

// -------------------------------------------------------------
//  404 → 302 到首页（Captive Portal 的关键一半）
// -------------------------------------------------------------
static void handleNotFound() {
  // 手机连上热点后会去探测 captive.apple.com / connectivitycheck.gstatic.com
  // 这类地址。通配 DNS 把它们指到本机，这里再统一 302 到配置页，
  // 系统就会认为「这个网络需要登录」并自动弹出。
  const String url = String("http://") + WiFi.softAPIP().toString() + "/";
  gServer.sendHeader(F("Location"), url, true);
  gServer.send(302, "text/plain", "");
}

// -------------------------------------------------------------
//  对外接口
// -------------------------------------------------------------
bool portalStart(const char* apSsid) {
  strncpy(gApSsid, apSsid, sizeof(gApSsid) - 1);
  gApSsid[sizeof(gApSsid) - 1] = '\0';
  gScanDone = false;

  // ★ 必须是 AP_STA：只有 STA 在才能 scanNetworks()。
  //   纯 AP 模式下扫描直接失败，页面下拉框会是空的。
  WiFi.mode(WIFI_AP_STA);
  delay(120);

  const char* pw = kPortalApPassword;
  const bool ok = (pw[0] == '\0') ? WiFi.softAP(gApSsid) : WiFi.softAP(gApSsid, pw);
  if (!ok) {
    Serial.println(F("[Portal] softAP 启动失败"));
    return false;
  }

  const IPAddress apIp = WiFi.softAPIP();

  // 通配 DNS：把 * 都解析到本机
  gDns.setErrorReplyCode(DNSReplyCode::NoError);
  gDns.start(kPortalDnsPort, "*", apIp);

  gServer.on("/", HTTP_GET, handleRoot);
  gServer.on("/save", HTTP_POST, handleSave);
  gServer.on("/forget", HTTP_GET, handleForget);
  gServer.on("/reboot", HTTP_GET, handleReboot);
  gServer.onNotFound(handleNotFound);
  gServer.begin();

  gActive = true;

  Serial.print(F("[Portal] 热点 "));
  Serial.print(gApSsid);
  Serial.print(F("  配网地址 http://"));
  Serial.println(apIp);

  // 启动时先扫一次，用户打开页面就能直接看到列表（不用再等 1~3 秒）
  scanAps();
  Serial.print(F("[Portal] 扫描到 "));
  Serial.print(gApCount);
  Serial.println(F(" 个网络"));

  return true;
}

void portalLoop() {
  if (!gActive) return;
  gDns.processNextRequest();
  gServer.handleClient();
}

void portalStop() {
  if (!gActive) return;
  gServer.stop();
  gDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gActive = false;
}

bool portalActive() { return gActive; }

const char* portalApSsid() { return gApSsid; }

IPAddress portalApIp() { return WiFi.softAPIP(); }
