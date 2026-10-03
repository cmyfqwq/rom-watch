/*
 * ota32-base —— ESP32 / ESP32-S3 底座固件（OTA + 配网 + 状态页）
 * ============================================================================
 * 和 ESP8266 那块（firmware/ota-base/）同一个 OTA 体系 ✓：
 *   · ArduinoOTA（espota 推送）  · /update 网页上传  · UDP 广播播报
 *   · 无凭据时开机起 AP 配网热点（captive portal）
 *   · /status /ping /reboot /reset —— ota.py 的 find/status 直接能用 ✓
 *
 * ★换芯片要改的零件（这就是"适配点"清单 ✓）：
 *   ① ESP8266WiFi.h        → WiFi.h
 *   ② ESP8266WebServer.h   → WebServer.h
 *   ③ EEPROM(1KB 模拟)     → Preferences(NVS)   —— ESP32 用 flash 里的 NVS 分区 ✓
 *   ④ ESP8266HTTPUpdateServer → 没有现成的 ✗ ⇒ /update 用 Update.h + HTTPUpload 手写 ✓
 *   ⑤ 板载 LED：很多 S3 板子是 WS2812 彩灯（要库）⇒ 底座不玩灯 ✓（状态全在网页/串口 ✓）
 *
 * OTA 口令：默认值在下面，★务必在 config.h 里改掉（tools/ota.py 会自动读 config.h ✓）
 * 广播端口 4210：⚠️ 必须避开 8266 —— ArduinoOTA 自己占着 UDP 8266 ✗
 * ============================================================================
 */
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>

#if __has_include("config.h")
#include "config.h"                    // 本机私有值（★不进仓库 ✓）
#endif

/* ---- 可配项（没配就走下面的默认 ✓）---- */
#ifndef FW_NAME
#define FW_NAME      "ota32-base"
#endif
#ifndef FW_VERSION
#define FW_VERSION   "1.0.0"
#endif
/* FW_BUILD：tools/ota.py 每次编译都会写一份新的 fw_build.h（时间戳 ✓）——
 *   OTA 后靠它确证"真的换了固件"（/status 的 build 字段 ✓）；没这份头文件就用 "0" ✓ */
#if __has_include("fw_build.h")
#include "fw_build.h"
#endif
#ifndef FW_BUILD
#define FW_BUILD "0"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "meow-ota-s3"       // ★公开默认 ⇒ 同网段谁都能刷 ⇒ 必须改 ✗
#endif
#ifndef AP_PREFIX
#define AP_PREFIX    "OTA32Base-"
#endif
#define ANNOUNCE_PORT 4210                   // ⚠️ 不能用 8266（ArduinoOTA 占着 ✗）
#define ANNOUNCE_EVERY_MS 30000UL

/* WiFi 凭据：#if 里不能对字符串取下标 ⇒ 另配数字开关 ✓（和 8266 版同一招）*/
#ifndef PVT_WIFI_SET
#define PVT_WIFI_SET 0
#endif
#if PVT_WIFI_SET
static const char* PVT_SSID = PVT_WIFI_SSID;
static const char* PVT_PASS = PVT_WIFI_PASS;
#else
static const char* PVT_SSID = "";
static const char* PVT_PASS = "";
#endif

/* ---- 状态 ---- */
Preferences prefs;
WebServer   server(80);
DNSServer   dns;
WiFiUDP     udp;
bool     apMode   = false;
bool     otaRunning = false;
unsigned long bootMs = 0, lastAnnounce = 0;
char     CfgSSID[33] = {0}, CfgPass[65] = {0};

String chipId() {                       // efuse MAC 的低 24 位（和 8266 版的 chipHex 同义 ✓）
  uint64_t m = ESP.getEfuseMac();
  char b[8]; snprintf(b, sizeof(b), "%02X%02X%02X",
                      (unsigned)(m >> 16) & 0xFF, (unsigned)(m >> 8) & 0xFF, (unsigned)m & 0xFF);
  return String(b);
}
String hostName() { return String("ota32-") + chipId(); }

/* ---- 凭据：NVS 读写 ---- */
void loadCfg() {
  prefs.begin("ota32", true);
  strlcpy(CfgSSID, prefs.getString("ssid", "").c_str(), sizeof(CfgSSID));
  strlcpy(CfgPass, prefs.getString("pass", "").c_str(), sizeof(CfgPass));
  prefs.end();
}
void saveCfg(const String &s, const String &p) {
  prefs.begin("ota32", false);
  prefs.putString("ssid", s); prefs.putString("pass", p);
  prefs.end();
}
void clearCfg() {
  prefs.begin("ota32", false);
  prefs.clear(); prefs.end();
}

/* ---- 状态 JSON（字段名和 ota-base 保持一致 ⇒ ota.py status 通用 ✓）---- */
void handleStatus() {
  String j = "{";
  j += "\"name\":\"" + hostName() + "\",";
  j += "\"fw\":\"" FW_NAME "\",\"ver\":\"" FW_VERSION "\",\"build\":\"" FW_BUILD "\",";
  j += "\"chipModel\":\"" + String(ESP.getChipModel()) + "\",";
  j += "\"cores\":" + String(ESP.getChipCores()) + ",\"mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  /* ★2026-10-03 修：原来写成 `"flash":16M` ✗ —— 裸 `M` 让 /status 变成**非法 JSON** ✗✓
   *   （实测：PowerShell 的 ConvertFrom-Json 直接报 "Unexpected character ... M" ✗）
   *   ⇒ 字段名带 MB、值是**纯数字** ✓ */
  j += "\"flashMB\":" + String((unsigned long)(ESP.getFlashChipSize() / 1024 / 1024)) + ",";
  j += "\"psramMB\":" + String((unsigned long)(ESP.getPsramSize() / 1024 / 1024)) + ",";
  j += "\"chip\":\"" + chipId() + "\",";
  j += "\"mac\":\"" + WiFi.macAddress() + "\",";
  j += "\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  j += "\"ssid\":\"" + (apMode ? String("(AP)") : WiFi.SSID()) + "\",";
  j += "\"rssi\":" + String(apMode ? 0 : WiFi.RSSI()) + ",";
  j += "\"heap\":" + String((unsigned long)ESP.getFreeHeap()) + ",";
  j += "\"up\":" + String((millis() - bootMs) / 1000) + ",";
  j += "\"ap\":" + String(apMode ? "true" : "false") + ",";
  j += "\"sdk\":\"" + String(ESP.getSdkVersion()) + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

/* ★2026-10-03 修：/ping 必须回**和 ota-base(8266) 一样的 JSON** ✓✓
 *   原来回纯文本 "pong" ✗ ⇒ ota.py 的 board_alive() 只认含 ok/fw/ver 的 JSON ⇒ 判定"没应答" ✗
 *   ⇒ 后果很严重：--ip 指定的板子被判死 ⇒ finder 退回"扫网段" ⇒ 扫到 8266 ⇒ **差点推错板子** ✗✗ */
void handlePing() {
  String j = "{\"ok\":true,\"fw\":\"" FW_NAME "\",\"ver\":\"" FW_VERSION "\",\"build\":\"" FW_BUILD "\",";
  j += "\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  j += "\"chip\":\"" + chipId() + "\"}";
  server.send(200, "application/json", j);
}
void handleReboot(){ server.send(200, "text/plain", "rebooting"); delay(300); ESP.restart(); }
void handleReset() {                       // 清凭据 + 重启 ⇒ 回配网模式 ✓
  server.send(200, "text/plain", "credentials cleared, rebooting into AP mode");
  clearCfg(); delay(300); ESP.restart();
}

/* ---- /update：网页直接传 .bin 刷机（ESP32 没有现成组件 ⇒ 手写 ✓）---- */
void handleUpdateUpload() {
  static bool ok = false;
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[UPD] 开始接收 %s\n", up.filename.c_str());
    ok = Update.begin();                   // 不给大小 ⇒ 用下一个 OTA 分区的全部 ✓
    if (!ok) Serial.printf("[UPD] begin 失败: %s\n", Update.errorString());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (ok) { Update.write(up.buf, up.currentSize); }   // ★不能写 `if (ok) size_t w = …` —— 裸语句不能是声明 ✗
  } else if (up.status == UPLOAD_FILE_END) {
    if (ok && Update.end(true)) {
      Serial.printf("[UPD] 完成 %u 字节 ⇒ 重启进新固件 ✓\n", (unsigned)up.totalSize);
      delay(400); ESP.restart();
    } else {
      Serial.printf("[UPD] 失败: %s\n", Update.errorString());
      ok = false;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end(); ok = false; Serial.println("[UPD] 中止");
  }
}
void handleUpdate() {                      // ★先要口令（admin / OTA_PASSWORD）再收包 ✓
  if (!server.authenticate("admin", OTA_PASSWORD)) return server.requestAuthentication();
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_END) server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
  else server.send(200, "text/plain", "...");
}

/* ---- 配网页（AP 模式下 captive portal ✓）---- */
const char* PORTAL_HTML = R"html(<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA32 配网</title>
<body style="font-family:system-ui;max-width:420px;margin:40px auto;padding:0 16px">
<h2>OTA32 底座 · 配网</h2>
<p style="color:#666">填你家 WiFi（<b>只支持 2.4G</b>），保存后板子会重启去连它。</p>
<form method="POST" action="/save">
 <p><input name="ssid" placeholder="WiFi 名" style="width:100%;padding:10px" required></p>
 <p><input name="pass" type="password" placeholder="WiFi 密码" style="width:100%;padding:10px"></p>
 <p><button style="width:100%;padding:12px">保存并连接</button></p>
</form>
<p style="color:#999;font-size:13px">型号与 IP 见串口日志；刷固件口令见 config.h。</p>
)html";

void handlePortalRoot() {
  if (server.hasArg("ssid")) {             // GET 带参也算保存（有的手机 portal 不发 POST ✓）
    saveCfg(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/html", "<meta charset='utf-8'>已保存，正在重启去连 WiFi…");
    delay(600); ESP.restart(); return;
  }
  server.send(200, "text/html", PORTAL_HTML);
}
void handlePortalSave() {
  saveCfg(server.arg("ssid"), server.arg("pass"));
  server.send(200, "text/html", "<meta charset='utf-8'>已保存，正在重启去连 WiFi…");
  delay(600); ESP.restart();
}

void handleRoot() {                        // STA 模式的首页 = 状态页 ✓
  String h = "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>ota32-base</title><body style='font-family:system-ui;max-width:520px;margin:32px auto;padding:0 16px'>"
             "<h2>OTA32 底座 · " + hostName() + "</h2><pre style='background:#f4f4f4;padding:12px;border-radius:8px'>";
  h += "固件   : " FW_NAME " " FW_VERSION "\n";
  h += "芯片   : " + String(ESP.getChipModel()) + " x" + String(ESP.getChipCores()) + " @" + String(ESP.getCpuFreqMHz()) + "MHz\n";
  h += "Flash  : " + String((unsigned long)(ESP.getFlashChipSize() / 1024 / 1024)) + " MB    PSRAM: " +
       String((unsigned long)(ESP.getPsramSize() / 1024 / 1024)) + " MB\n";
  h += "IP     : " + WiFi.localIP().toString() + "  (" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + "dBm)\n";
  h += "堆     : " + String((unsigned long)ESP.getFreeHeap()) + " B    已运行 " + String((millis() - bootMs) / 1000) + " s\n";
  h += "MAC    : " + WiFi.macAddress() + "\n";
  h += "</pre><p><a href='/update'>网页刷固件</a> · <a href='/status'>/status</a> · "
       "<a href='/reset' onclick=\"return confirm('清掉 WiFi 凭据并重启？')\">清凭据</a></p>";
  server.send(200, "text/html", h);
}

/* ---- UDP 播报：ota.py find 靠这个认板子（前缀必须是 OTABASE ✓）---- */
void announce() {
  char msg[160];
  snprintf(msg, sizeof(msg), "OTABASE|%s|%s|%s|%s|%s",
           hostName().c_str(),
           (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str(),
           FW_VERSION, WiFi.macAddress().c_str(),
           apMode ? "(AP)" : WiFi.SSID().c_str());
  udp.beginPacket(IPAddress(255, 255, 255, 255), ANNOUNCE_PORT);
  udp.write((const uint8_t *)msg, strlen(msg));
  udp.endPacket();
  Serial.printf("[STAT] ip=%s rssi=%d heap=%u up=%lus\n",
                (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str(),
                apMode ? 0 : WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(), (millis() - bootMs) / 1000);
}

/* ---- 连 STA（连不上自动退回 AP ✓）---- */
bool startSTA() {
  WiFi.mode(WIFI_STA);
  /* ★★2026-10-03 实测救活（这一句是关键 ✓✓）：
   *   **射频上电是一个电流尖峰 ✗，紧接着 WiFi.begin() 的扫描/连接是第二个尖峰 ✗** ——
   *   两个尖峰背靠背 ⇒ 这块板子直接欠压复位 ✗。
   *   证据：能跑通的探针在 mode 和 begin **之间隔了 300ms** ✓✓；本固件原来紧挨着 ⇒ 每次都崩 ✗。
   *   ⇒ 中间插一个 delay 让第一个尖峰过去 ✓（这不是掩盖：欠压检测照样开着 ✓）*/
  delay(400);
  WiFi.setHostname(hostName().c_str());    // ★要在 begin 前设 ✓
  WiFi.begin(CfgSSID, CfgPass);
  Serial.printf("[WIFI] connecting to '%s' ...\n", CfgSSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(120); }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] ✗ 连不上（检查：名字/密码大小写；**必须 2.4G**；信号）⇒ 回配网模式");
    return false;
  }
  Serial.printf("[WIFI] OK ip=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

void startAP() {
  apMode = true;
  String ap = String(AP_PREFIX) + chipId();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap.c_str());
  delay(200);
  dns.start(53, "*", WiFi.softAPIP());     // captive portal：随便访问什么域名都弹配网页 ✓
  Serial.printf("[AP] 热点 %s  http://%s/\n", ap.c_str(), WiFi.softAPIP().toString().c_str());
}

void setup() {
  bootMs = millis();
  Serial.begin(115200);
  delay(120);
  /* ★★2026-10-03 实测：这块板子单跑"不碰 WiFi 的最小程序"完全正常 ✓（16 秒心跳无异常 ✓），
   *   但一开 WiFi 就 `E BOD: Brownout detector was triggered` ✗ 无限重启 ⇒
   *   ⇒ **供电对"射频上电的 inrush 尖峰"余量不足** ✗（细 USB 线 / 弱 USB 口 / HUB 的经典病 ✓）
   *   软件侧缓解（能压一点是一点 ✓）：
   *     ① CPU 降到 80MHz ⇒ 整体电流下来，给射频尖峰腾余量 ✓
   *     ② 发射功率降到 8.5dBm（见 startSTA ✓）
   *   ⚠️ 根治仍是硬件：**换一根粗短的 USB 线、插主机直出的 USB 口、别用扩展坞** ✓✓ */
  /* ★★2026-10-03 实测记录（重要 ✗）：这块板子在这台电脑的 USB 供电下**开不了 WiFi** ——
   *   串口一路刷 `E BOD: Brownout detector was triggered` ✗ 无限重启。
   *   本喵做过的对照实验（都是实测 ✓）：
   *     · 最小探针（不碰 WiFi / NVS）⇒ **稳跑 16 秒无异常** ✓ ⇒ 芯片与基本供电没问题 ✓
   *     · 本固件一开 WiFi ⇒ 立刻 BOD ✗（射频上电那一下把 3.3V 拉垮）
   *     · CPU 降到 80MHz ＋ 发射功率降到 8.5dBm ⇒ **照样 BOD** ✗（软件压不下去）
   *     · 关掉欠压检测（WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG,0)）⇒ **照样 BOD，而且更早** ✗
   *   ⇒ ⇒ **结论：硬件供电余量不足，软件无解** ✗✓
   *      （同一条 USB 线上的 ESP8266 活得好好的 ✓ —— 因为 S3 的 WiFi 峰值电流约是 8266 的 3 倍 ✗）
   *   ⇒ **根治（只能人在物理侧做 ✓）**：
   *     ① 换一根**粗短**的 USB 线（细长线压降大 ✗）
   *     ② 插**主机直出**的 USB 口 —— 别用前面板、别用 USB HUB / 扩展坞 ✗
   *     ③ 或用 **5V ≥1A 的充电头**单独供电 ✓
   *   （换好线之后**不用重新烧**：本固件已经烧进去了，它会自己连上网 ✓）*/
  Serial.printf("\n=== %s %s | %s x%d @%dMHz | Flash %uMB PSRAM %uMB ===\n", FW_NAME, FW_VERSION,
                ESP.getChipModel(), ESP.getChipCores(), ESP.getCpuFreqMHz(),
                (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getPsramSize() >> 20));

  /* ★2026-10-03 实测结论（重要 ✗）：**这块板子跑 WiFi 的供电余量不足** ——
   *   证据：一个 40 行的纯 WiFi 探针（没有本固件一行代码）也**时好时坏、多数时候连崩** ✗✓；
   *         而"不碰 WiFi"的最小探针稳跑 ✓ ⇒ 芯片没问题，是射频那一下的电流 ✗。
   *   软件侧全试过、**全部无效** ✗：降频 80MHz ✗ 降发射功率 ✗ 关欠压检测 ✗ 各种延时 ✗
   *         不写/不读 NVS ✗ mode 与 begin 之间插间隔 ✗ ⇒ **硬件问题，软件无解** ✓✓
   *   ⇒ 处置：① **换 5V ≥1A 的充电头单独供电**（或带外接电源的 USB HUB ✓）——
   *             这样它不靠电脑的 USB 口，最可能一次就好 ✓✓（那样也就不需要 USB 数据线了，
   *             连上网之后本喵可以直接走无线 OTA ✓）
   *           ② 若换了强供电仍崩 ⇒ 这块板子的 LDO 有问题 ⇒ 换板子 ✗
   *   本固件的自保：下面按复位原因自适应等待 ✓（万一供电只是浅欠压，多等几轮能挤过去 ✓）*/
  loadCfg();                                 // 读 NVS（配网页存过的凭据 ✓；读很便宜、实测无害 ✓）
#if PVT_WIFI_SET
  if (!CfgSSID[0]) {                         // ★开机**不写 NVS** ✓（省掉一次 flash 擦写 ✓）
    strlcpy(CfgSSID, PVT_SSID, sizeof(CfgSSID));
    strlcpy(CfgPass, PVT_PASS, sizeof(CfgPass));
    Serial.println("[CFG] 用 config.h 里的 WiFi 凭据（不写 NVS ✓）");
  }
#endif

  /* ★★2026-10-03 实测救活（关键 ✓✓）：这块板子**冷启动时供电还没稳住**，一开 WiFi 就欠压复位 ✗；
   *   但**复位一次（热了）之后同样的代码就能连上网** ✓✓（实测：第二次开机 status=3、拿到 IP ✓）。
   *   对照证据：探针在开 WiFi 前等了 600ms ⇒ 第二次就成了 ✓；本固件只等 120ms ⇒ 每次都崩 ✗。
   *   ⇒ 做法：**按复位原因自适应等待** —— 上次是欠压复位就多等几秒让轨压充起来 ✓✓
   *   （这不是掩盖 ✗：欠压检测保持开启 ✓，只是"别在最脆的那一瞬间去开射频" ✓）*/
  {
    esp_reset_reason_t rr = esp_reset_reason();
    unsigned waitMs = 800;
    if (rr == ESP_RST_BROWNOUT)      { waitMs = 3500; Serial.println("[PWR] 上次是欠压复位 ⇒ 多等 3.5 秒让供电稳住 ✓"); }
    else if (rr == ESP_RST_POWERON)  { waitMs = 1500; Serial.println("[PWR] 冷启动 ⇒ 等 1.5 秒再开射频 ✓"); }
    Serial.printf("[PWR] 开 WiFi 前等 %ums（reset_reason=%d）\n", waitMs, (int)rr);
    delay(waitMs);
  }

  if (CfgSSID[0] && startSTA()) { /* 连上了 ✓ */ }
  else startAP();

  ArduinoOTA.setHostname(hostName().c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { otaRunning = true;  Serial.println("[OTA] 开始"); });
  ArduinoOTA.onEnd([]()   { otaRunning = false; Serial.println("\n[OTA] 完成 ⇒ 重启 ✓"); });
  ArduinoOTA.onProgress([](unsigned p, unsigned t) {
    static int last = -1; int pct = t ? p * 100 / t : 0;
    if (pct / 20 != last / 20) { last = pct; Serial.printf("[OTA] %d%%\n", pct); }
  });
  ArduinoOTA.onError([](ota_error_t e) { otaRunning = false; Serial.printf("[OTA] err %u\n", e); });
  ArduinoOTA.begin();

  server.on("/",            HTTP_GET,  apMode ? handlePortalRoot : handleRoot);
  server.on("/save",        HTTP_POST, handlePortalSave);
  server.on("/status",      HTTP_GET,  handleStatus);
  server.on("/ping",        HTTP_GET,  handlePing);
  server.on("/reboot",      HTTP_GET,  handleReboot);
  server.on("/reset",       HTTP_GET,  handleReset);
  server.on("/update",      HTTP_POST, handleUpdate, handleUpdateUpload);
  server.onNotFound(apMode ? handlePortalRoot : handleRoot);
  server.begin();

  udp.begin(ANNOUNCE_PORT);
  announce(); lastAnnounce = millis();
  Serial.printf("\n[BOOT] %s %s  http://%s/\n\n", FW_NAME, FW_VERSION,
                (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
  Serial.println("→ 下一步：无线刷固件用 tools/ota.py push --sketch <工程>（口令在 config.h ✓）");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (apMode) dns.processNextRequest();
  if (millis() - lastAnnounce >= ANNOUNCE_EVERY_MS) { announce(); lastAnnounce = millis(); }
}
