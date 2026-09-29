/*
 * ota-base —— ESP8266 OTA 底座固件（WeMos D1 mini / ESP-12F / 4MB）
 * ------------------------------------------------------------------
 * 只干四件事，保持极简、极稳（业务固件以后叠在上面）：
 *   1) 连 WiFi：凭据存 EEPROM；连不上 → 自己开热点配网（captive portal）
 *   2) 三条 OTA 通道：
 *        a) ArduinoOTA  ← espota.py 推送（PC 侧 ota.py push 走这条）
 *        b) 网页上传    ← http://<ip>/update （浏览器兜底）
 *        c) 拉取模式    ← http://<ip>/pull?host=<PC>&port=8080&path=/fw.bin
 *   3) 主动播报：串口打印 IP + UDP 广播（255.255.255.255:8266），绕开 mDNS
 *   4) LED 状态：快闪=配网中 / 慢闪=连WiFi中 / 短闪=已连 / 常亮=正在刷
 *
 * 首次必须串口刷入（FT232 / COM5），之后就能全程无线。
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <DNSServer.h>

// 由 tools\ota.py 每次编译写入的 FW_BUILD 时间戳（用来确证"OTA 后真的换了固件"）
#if __has_include("fw_build.h")
#include "fw_build.h"
#endif

/* ★2026-09-30：本机私有配置（跟 rom-watch 同一口径）——
 *   有 `config.h` 就用它（**OTA 口令只写那一处** ✓），**没有也能编**（走下面的公开默认值 ✓）。
 *   为什么加：这块是"保命底座"，若它的口令还是公开默认值，那刷一次底座就把刚换的口令又打回原形 ✗ */
#if __has_include("config.h")
#include "config.h"
#endif
#ifndef FW_BUILD
#define FW_BUILD "dev"
#endif

#define FW_NAME       "ota-base"
#define FW_VERSION    "0.1.0"
/* ★2026-09-30（开源准备）：口令做成**可覆盖**（`#ifndef`）—— 默认值不变、行为不变 ✓，
 *   但想换口令的人**不用改源码**，在命令行加 -DOTA_PASSWORD=\"你的口令\" 或另放一个
 *   config.h 即可 ✓（跟 rom-watch 的适配层同一口径 ✓）*/
#ifndef OTA_PASSWORD
#define OTA_PASSWORD  "meow-ota-8266"     // espota -a / 网页上传 basic auth
#endif
#define AP_PREFIX     "OTABase-"
#define ANNOUNCE_PORT 4210                // ⚠️ 必须避开 8266：ArduinoOTA 自己占着 UDP 8266，
                                          //    撞车会把 OTA 的握手包抢走（实测过 espota "No Answer"）
#define CFG_MAGIC     0x4F544131UL        // "OTA1"
#define LED_PIN       LED_BUILTIN         // D4/GPIO2，低电平点亮
#define WIFI_TIMEOUT_MS 25000UL           // 连 WiFi 最长等 25 秒
#define ANNOUNCE_EVERY_MS 30000UL         // 每隔 30 秒广播一次

struct Cfg {
  uint32_t magic;
  char     ssid[33];
  char     pass[65];
} cfg;

ESP8266WebServer        server(80);
ESP8266HTTPUpdateServer httpUpdater;
DNSServer               dns;
WiFiUDP                 udp;

bool          apMode       = false;
bool          otaRunning   = false;
unsigned long lastAnnounce = 0;
unsigned long lastBeat     = 0;
uint32_t      bootMs       = 0;

String chipHex() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%06X", ESP.getChipId());
  return String(buf);
}

String hostName() {
  return String(FW_NAME) + "-" + chipHex();
}

// ---------------------------------------------------------------- LED
void ledOn()  { digitalWrite(LED_PIN, LOW);  }   // 低电平点亮
void ledOff() { digitalWrite(LED_PIN, HIGH); }

void ledBlink(int ms) {
  ledOn();  delay(ms);
  ledOff(); delay(ms);
}

// ---------------------------------------------------------------- 配置存取
void saveCfg(const String &ssid, const String &pass) {
  cfg.magic = CFG_MAGIC;
  memset(cfg.ssid, 0, sizeof(cfg.ssid));
  memset(cfg.pass, 0, sizeof(cfg.pass));
  strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
  strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void clearCfg() {
  cfg.magic = 0;
  cfg.ssid[0] = 0;
  cfg.pass[0] = 0;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------------------------------------------------------- 配网页
String pageHeader(const String &title) {
  String h = F("<!doctype html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>");
  h += title;
  h += F("</title><style>body{font-family:system-ui,sans-serif;margin:16px;max-width:560px}"
         "h2{margin:.2em 0}.c{background:#111;color:#0f0;padding:8px;border-radius:6px;"
         "font-family:monospace;font-size:13px;white-space:pre-wrap}"
         "input,select,button{font-size:16px;padding:8px;margin:6px 0;width:100%;box-sizing:border-box}"
         "button{background:#2b7;color:#fff;border:0;border-radius:6px}"
         "a{color:#2b7}</style></head><body>");
  return h;
}

String infoBlock() {
  String s;
  s += "chip   : ESP8266 " + chipHex() + "\n";
  s += "firmware: " + String(FW_NAME) + " " + String(FW_VERSION) + "\n";
  s += "build  : " + String(FW_BUILD) + "\n";
  s += "mac    : " + WiFi.macAddress() + "\n";
  if (!apMode) {
    s += "ssid   : " + WiFi.SSID() + "\n";
    s += "ip     : " + WiFi.localIP().toString() + "\n";
    s += "rssi   : " + String(WiFi.RSSI()) + " dBm\n";
  }
  s += "heap   : " + String(ESP.getFreeHeap()) + " B\n";
  s += "uptime : " + String((millis() - bootMs) / 1000) + " s\n";
  s += "reset  : " + ESP.getResetReason() + "\n";
  return s;
}

void handlePortalRoot() {
  int n = WiFi.scanNetworks();
  String html = pageHeader("OTA 底座配网");
  html += F("<h2>OTA 底座配网</h2><form method='POST' action='/save'>");
  html += F("<label>WiFi</label><select name='s'>");
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    html += "<option value='" + ssid + "'>" + ssid + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
  }
  html += F("</select><input name='s2' placeholder='(列表里没有？手动填 SSID)'>");
  html += F("<label>密码</label><input name='p' type='password' placeholder='WiFi 密码'>");
  html += F("<button type='submit'>保存并重启</button></form>");
  html += F("<div class='c'>当前状态\n");
  html += infoBlock();
  html += F("</div></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handlePortalSave() {
  String ssid = server.arg("s2");
  if (ssid.length() == 0) ssid = server.arg("s");
  String pass = server.arg("p");
  if (ssid.length() == 0) {
    server.send(200, "text/html; charset=utf-8",
                pageHeader("失败") + F("<h2>SSID 空了</h2><a href='/'>返回</a>"));
    return;
  }
  saveCfg(ssid, pass);
  String html = pageHeader("已保存");
  html += F("<h2>已保存，正在重启…</h2><div class='c'>SSID: ");
  html += ssid;
  html += F("</div><p>重启后会尝试连接；连不上会再次开热点。</p></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
  delay(800);
  ESP.restart();
}

// ---------------------------------------------------------------- STA 侧页面
void handleRoot() {
  String html = pageHeader("OTA 底座");
  html += F("<h2>OTA 底座</h2><div class='c'>");
  html += infoBlock();
  html += F("</div><p><a href='/update'>→ 网页上传固件 (/update)</a></p>"
            "<p><a href='/reset'>→ 清除 WiFi 凭据并重启</a></p></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleStatus() {
  String j = "{";
  j += "\"name\":\"" + hostName() + "\",";
  j += "\"fw\":\"" + String(FW_NAME) + "\",";
  j += "\"ver\":\"" + String(FW_VERSION) + "\",";
  j += "\"build\":\"" + String(FW_BUILD) + "\",";
  j += "\"chip\":\"" + chipHex() + "\",";
  j += "\"mac\":\"" + WiFi.macAddress() + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"ssid\":\"" + WiFi.SSID() + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"up\":" + String((millis() - bootMs) / 1000) + ",";
  j += "\"reset\":\"" + ESP.getResetReason() + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

void handlePing() {
  server.send(200, "application/json",
              "{\"ok\":true,\"fw\":\"" + String(FW_NAME) + "\",\"ver\":\"" + String(FW_VERSION) +
              "\",\"build\":\"" + String(FW_BUILD) + "\",\"ip\":\"" + WiFi.localIP().toString() +
              "\",\"chip\":\"" + chipHex() + "\"}");
}

void handleReset() {
  clearCfg();
  server.send(200, "text/html; charset=utf-8",
              pageHeader("已清除") + F("<h2>凭据已清除，重启进配网模式</h2></body></html>"));
  delay(600);
  ESP.restart();
}

// 拉取式 OTA：/pull?host=<你电脑的IP>&port=8080&path=/fw.bin
void handlePull() {
  String host = server.arg("host");
  int    port = server.arg("port").toInt();
  String path = server.arg("path");
  if (path.length() == 0) path = "/fw.bin";
  if (host.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing host\"}");
    return;
  }
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"pulling\",\"from\":\"" + host + ":" + String(port) + path + "\"}");
  delay(300);
  Serial.printf("[PULL] %s:%d%s\n", host.c_str(), port, path.c_str());
  WiFiClient client;
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, host, port, path, "");
  switch (ret) {
    case HTTP_UPDATE_FAILED: Serial.printf("[PULL] FAILED (%d) %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str()); break;
    case HTTP_UPDATE_NO_UPDATES: Serial.println("[PULL] NO_UPDATES"); break;
    case HTTP_UPDATE_OK: Serial.println("[PULL] OK, rebooting"); break;   // 成功后库里自己重启
  }
}

// ---------------------------------------------------------------- 播报
void announce() {
  char msg[160];
  snprintf(msg, sizeof(msg), "OTABASE|%s|%s|%s|%s|%s",
           hostName().c_str(), WiFi.localIP().toString().c_str(), FW_VERSION,
           WiFi.macAddress().c_str(), WiFi.SSID().c_str());
  udp.beginPacket(IPAddress(255, 255, 255, 255), ANNOUNCE_PORT);
  udp.write((const uint8_t *)msg, strlen(msg));
  udp.endPacket();
  Serial.printf("[STAT] ip=%s rssi=%d heap=%u up=%lus\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                ESP.getFreeHeap(), (millis() - bootMs) / 1000);
}

// ---------------------------------------------------------------- 模式启动
void startSTA() {
  apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostName());
  WiFi.begin(cfg.ssid, cfg.pass);
  Serial.printf("[WIFI] connecting to '%s' ...\n", cfg.ssid);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    ledBlink(250);
    delay(1);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] FAILED -> 进入配网模式");
    startAP();
    return;
  }

  Serial.printf("[WIFI] OK  ip=%s  rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  httpUpdater.setup(&server, "/update", "admin", OTA_PASSWORD);
  server.on("/",       handleRoot);
  server.on("/status", handleStatus);
  server.on("/ping",   handlePing);
  server.on("/reset",  handleReset);
  server.on("/pull",   handlePull);
  server.begin();

  ArduinoOTA.setHostname(hostName().c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { otaRunning = true; ledOn(); Serial.println("[OTA] start"); });
  ArduinoOTA.onEnd([]()   { otaRunning = false; Serial.println("\n[OTA] end"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    static int last = -1;
    int pct = t ? (int)(p * 100 / t) : 0;
    if (pct / 10 != last / 10) { last = pct; Serial.printf("[OTA] %d%%\n", pct); }
  });
  ArduinoOTA.onError([](ota_error_t e) { otaRunning = false; Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();

  udp.begin(ANNOUNCE_PORT);
  announce();
  lastAnnounce = millis();
  Serial.printf("\n[BOOT] %s %s  已就绪：http://%s/   (espota 密码 %s)\n\n",
                FW_NAME, FW_VERSION, WiFi.localIP().toString().c_str(), OTA_PASSWORD);
  ledOn(); delay(120); ledOff();
}

void startAP() {
  apMode = true;
  String apSsid = String(AP_PREFIX) + chipHex();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str());                 // 开放热点，仅配网期存在
  delay(300);
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);
  server.on("/",       handlePortalRoot);
  server.on("/save",   HTTP_POST, handlePortalSave);
  server.onNotFound([]() { handlePortalRoot(); });
  server.begin();
  Serial.printf("\n[AP] 配网热点：%s   打开 http://%s/\n\n", apSsid.c_str(), ip.toString().c_str());
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledOff();
  Serial.begin(115200);
  delay(200);
  bootMs = millis();
  Serial.printf("\n\n=== %s %s build %s (chip %s) ===\n",
                FW_NAME, FW_VERSION, FW_BUILD, chipHex().c_str());

  EEPROM.begin(512);
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC || cfg.ssid[0] == 0) {
    Serial.println("[CFG] 没有凭据 -> 配网模式");
    memset(&cfg, 0, sizeof(cfg));
    startAP();
  } else {
    startSTA();
  }
}

void loop() {
  if (apMode) {
    dns.processNextRequest();
    server.handleClient();
    // 快闪表示在等人配网
    static unsigned long t = 0;
    if (millis() - t > 150) { t = millis(); digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
    return;
  }

  ArduinoOTA.handle();
  server.handleClient();

  if (millis() - lastAnnounce > ANNOUNCE_EVERY_MS) { announce(); lastAnnounce = millis(); }

  // 心跳短闪（OTA 进行中则常亮）
  if (!otaRunning && millis() - lastBeat > 3000) {
    lastBeat = millis();
    ledOn(); delay(30); ledOff();
  }

  // 掉线重连
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] 掉线，重连…");
      WiFi.reconnect();
    }
  }
}
