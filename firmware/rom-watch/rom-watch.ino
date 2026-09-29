/*
 * rom-watch —— ESP8266「新版本」监测看板（网盘目录 / GitHub Release 都行）
 * 默认板型：WeMos D1 mini（ESP-12F / 4MB）。换板子只改 config.h 的适配层 ✓
 * ------------------------------------------------------------------
 * 在 ota-base 底座之上叠业务：
 *   1) 定时抓 OpenList 的目录 API（HTTPS / chunked / 手写流式解析，不依赖 JSON 库）
 *   2) 取「上传时间最新的那个版本目录」= 最新包
 *   3) 跟「水位线（已确认版本）」比对 → 不一样就报警（LED 快闪 + 网页高亮）
 *   4) 网页上三个动作：我已刷 ✓（推进水位线）/ 跳过这版 / 立即检查
 *
 * 保留底座的一切：AP 配网（EEPROM 凭据与 ota-base 同布局，升级不用重配）、
 * ArduinoOTA 推送、网页 /update、拉取 /pull、UDP 广播、/status /ping /reset
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>                          // NTP 对时（屏上/网页显示真实时间 ✓）
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <DNSServer.h>

/* ===== 本机私有配置（编译开关 + 你的默认值）都在 config.h 里 =====
 * ★为什么这么切：①之前 IRAM 已经 96%（63475/65536）⇒ 再加一点功能就编不过 ✗，
 *   现在把"串口屏 / OLED"做成**编译期开关** ⇒ 不用哪个就整块不编进来，IRAM/flash 立刻松 ✓
 *   ②你的 WiFi 凭据/私有服务器地址以前散在主文件里 ⇒ 集中到一个文件，将来要公开就换掉它一个文件 ✓
 *   （config.h 用 __has_include 探测 ⇒ **没有它也能编**，只是走中性默认值＋必须配网 ✓）*/
#if __has_include("config.h")
#include "config.h"
#endif
#ifndef USE_TJC
#define USE_TJC 1                          // 串口屏（SoftwareSerial D5/D7 + 坐标驱动）
#endif
#ifndef USE_OLED
#define USE_OLED 1                         // 0.96" OLED（含中文字模，吃 flash）
#endif
#ifndef PVT_WIFI_SET
#define PVT_WIFI_SET 0                     // 没有 config.h ⇒ 不写死凭据，开机走 AP 配网 ✓
#endif
#ifndef PVT_WIFI_SSID
#define PVT_WIFI_SSID ""                   // 私有默认 WiFi（空 = 开机走 AP 配网 ✓）
#endif
#ifndef PVT_WIFI_PASS
#define PVT_WIFI_PASS ""
#endif
/* ★2026-09-30（主人原话：「接着维护当前项目适配性系统 记得我这个项目要开源的」）：
 *   从这一行往下 = **适配层** —— 全部写成 `#ifndef`，三个好处：
 *   ① 没有 config.h 也能编 ✓（走中性默认值 + 开机配网）
 *   ② 要换接线/换屏/换数据源，**只改 config.h 一个文件**，主文件一个字不动 ✓✓
 *   ③ 默认值刻意取**中性的**（空数据源 + D1 mini 标准接线）⇒ 直接公开也不泄露私有信息 ✓✓
 *      （原来这里写死了主人的私有域名和目录 ✗ —— 那是私有地址，
 *        主人自己的机器靠 config.h 提供，所以改成空也不会影响他 ✓）*/
#ifndef DFLT_HOST
#define DFLT_HOST   ""                     // 空 = 还没配数据源（网页「监测设置」里填 ✓）
#endif
#ifndef DFLT_PATH
#define DFLT_PATH   ""                     // 同上（例：OpenList 的目录 /public/roms ✓）
#endif
#ifndef DFLT_INTERVAL
#define DFLT_INTERVAL 600                  // 轮询秒（网页最小 60 ✓）
#endif
#ifndef DFLT_TITLE
/* ★2026-09-30（主人：「开源的我觉得这个系统包监测这几个字应该能让人改」）：
 *   网页 / OLED / 串口屏**三处共用的标题**，在这里可配 ✓（网页「监测设置 → 标题」也能随时改 ✓）
 *   ⚠️ **标题里的每个字都必须在字模表里**，否则屏上会画成**空方框** ✗
 *     ⇒ 换了标题、里面有新字，就把那些字写进 `cn_extra.txt` 再重跑 `qwq\gen_cn_font.ps1` ✓✓ */
#define DFLT_TITLE  "系统包监测"
#endif

/* ---- 硬件适配：接线 / 显示屏 / 按键（都有默认值，不改也能跑 ✓）---- */
#ifndef PIN_OLED_SDA
#define PIN_OLED_SDA  D2                   // I2C 数据（OLED）
#endif
#ifndef PIN_OLED_SCL
#define PIN_OLED_SCL  D1                   // I2C 时钟
#endif
#ifndef OLED_ADDR
#define OLED_ADDR     0x3C                 // 常见 0x3C；少数模块是 0x3D
#endif
#ifndef OLED_SMALL
#define OLED_SMALL    0                    // 0 = 128x64（默认）  1 = 128x32（0.91" 那种，版面自动换成两行 ✓）
#endif
#ifndef PIN_TJC_RX
#define PIN_TJC_RX    D5                   // 串口屏 TX → 板 D5
#endif
#ifndef PIN_TJC_TX
#define PIN_TJC_TX    D7                   // 串口屏 RX → 板 D7（★别用 D6：那是开机判 flash 电压的脚）
#endif
#ifndef PIN_LED
#define PIN_LED       LED_BUILTIN          // 板载 LED（低电平点亮）
#endif
#ifndef PIN_BTN
#define PIN_BTN       D3                   // 板载 FLASH 键（按下为低）
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD  "meow-ota-8266"      // ★开源后请改成你自己的（在 config.h 里覆盖 ✓）
#endif

/* 把宏"变成字符串"（给 /i2c 这类端点**回显真实引脚名**用 ✓）——
 * ★必须两级：`RW_STR(PIN_OLED_SDA)` 才会展开成 "D2"；单级只会得到 "PIN_OLED_SDA" ✗
 * 起因（2026-09-30 子代理审计发现）：/i2c 原来把 `"sda":"D2"` **写死在字符串里** ✗
 *   ⇒ 别人按适配层把引脚改成别的，/i2c 还在报 D2 ⇒ 排查被带偏 ✗✓ */
#define RW_STR2(x) #x
#define RW_STR(x)  RW_STR2(x)

#if USE_TJC
#include <SoftwareSerial.h>
#endif
#if USE_OLED
#include <SSD1306Wire.h>                   // ThingPulse SSD1306 驱动（自带 ASCII 字体）
#include "cn_font.h"                       // 中文点阵（宋体 16/24px，由 gen_cn_font.ps1 生成）
#endif

#if __has_include("fw_build.h")
#include "fw_build.h"
#endif
#ifndef FW_BUILD
#define FW_BUILD "dev"
#endif

#define FW_NAME       "rom-watch"
#define FW_VERSION    "1.0.0"                 // ★2026-09-30：从 0.3.1 改成 1.0.0 —— 这是**第一个公开版本**
                                              //   （原来那串是主人自己迭代的序号，不是"公开版号" ✓）
/* OTA_PASSWORD 不在这里写死 —— 它是**可配的适配项**，见上面适配层的 #ifndef ✓ */
#define AP_PREFIX     "ROMWatch-"
#define ANNOUNCE_PORT 4210                 // 不能是 8266（ArduinoOTA 占着）
#define CFG_MAGIC     0x4F544131UL         // 与 ota-base 一致 → WiFi 凭据沿用
#define MON_MAGIC     0x524F4D31UL         // "ROM1"
#define CFG_OFF       0
#define MON_OFF       128
#define LED_PIN       PIN_LED               // 都走适配层 ⇒ 换板子只改 config.h ✓
#define BTN_PIN       PIN_BTN

struct Cfg {                               // 与 ota-base 完全同布局
  uint32_t magic;
  char     ssid[33];
  char     pass[65];
} cfg;

struct Mon {
  uint32_t magic;
  uint32_t interval;                       // 轮询间隔秒
  char     host[48];
  char     path[96];
  char     ack[24];                        // 水位线：已刷的版本
  char     skip[24];                       // 被跳过的版本
  char     latest[24];                     // 上次抓到的最新版本
  char     latestTime[20];                 // 该版本上传时间(前19字符)
  uint32_t lastOkAt;                       // 上次抓成功（uptime 秒）
  uint32_t failCount;
  char     lastErr[48];
} mon;

ESP8266WebServer        server(80);
ESP8266HTTPUpdateServer httpUpdater;
DNSServer               dns;
WiFiUDP                 udp;
BearSSL::WiFiClientSecure tls;
/* ★2026-09-26 串口屏接线（主人指定）：屏 TX → D5(GPIO14，板的 RX)；屏 RX → D7(GPIO13，板的 TX)。
 * 原来写的 D6(GPIO12) ✗ —— 那是**开机判 flash 电压**的脚，被串口占着不合适 ⇒ 换到 D7 ✓ */
#if USE_TJC
SoftwareSerial          tjc(PIN_TJC_RX, PIN_TJC_TX);   // 引脚走适配层（config.h 可覆盖）✓
#else
/* ★没开串口屏（USE_TJC=0）时：下面那些 tjc* 函数全部退化成**空函数** —— 
 *   目的是**调用点一个字都不用改** ✓（都写成一个样，链接器会把没被引用的丢掉 ✓）*/
#endif
#if USE_OLED
/* 地址/引脚/尺寸全部走适配层 ✓ —— OLED_SMALL=1 时自动换成 128x32 那种 0.91" 屏 ✓ */
SSD1306Wire             oled(OLED_ADDR, PIN_OLED_SDA, PIN_OLED_SCL,
                             OLED_SMALL ? GEOMETRY_128_32 : GEOMETRY_128_64);
#define OLED_H (OLED_SMALL ? 32 : 64)      // 版面高度（oledUpdate/stDraw 靠它分岔 ✓）
#endif

/* 这三个**任何配置下都保留**：网页的 /rom 与 /status 都要报它们 ✓（没开屏就恒为空/0 ✓） */
String tjcInfo = "";                       // 屏的 connect 回包（空=没接屏/没应答）
long   tjcBaud  = USE_TJC ? 115200 : 0;    // ★探测到的屏波特率；**没编屏就报 0**（否则网页会说"无应答（探测 115200）"✗ 那是假信息）
bool   tjcReady = false;                   // tjc.begin() 之后才允许往屏上发 ✓（随 USE_TJC 一起工作 ✓）
#if USE_TJC
String tjcSsid[10];                        // ★#SCAN 时填：wifi 页第 N 行对应哪个 SSID（坐标驱动用 ✓）

// 给串口屏发一条 TJC 指令（自动补 FF FF FF），返回屏的回包
String tjcCmd(const String &cmd, uint16_t waitMs = 400) {
  while (tjc.available()) tjc.read();
  tjc.print(cmd);
  tjc.write(0xFF); tjc.write(0xFF); tjc.write(0xFF);
  String resp;
  unsigned long t0 = millis();
  while (millis() - t0 < waitMs) {
    while (tjc.available()) { resp += (char)tjc.read(); t0 = millis(); }
  }
  return resp;
}

/* ===== UTF-8 → GB2312（★发中文前必须过这一道 ✓）=====
 * ★TJC（陶晶驰 X 系列）**内置字库是 GB2312** ✗ —— 直接把 UTF-8 汉字发过去，屏上是一片**乱码** ✗✓。
 * 这里带一张"够用就好"的映射表（PROGMEM，只覆盖本文件推送文案里用到的字符 ✓）：
 *   汉字/全角标点 → GB2312 双字节 ✓；表里没有的字符退化成 '?'（既不会崩，也不会发出 0xFF 破坏帧尾 ✓）。
 * 表里的条目由脚本按"本文件**字符串字面量**里出现的所有非 ASCII 字符"生成 ⇒ **改文案后要重跑脚本** ✓
 * 注：GB2312 的字节范围是 A1~FE，**永远不会出现 0xFF** ⇒ 不会和 TJC 的帧尾撞车 ✓✓ */
struct GbPair { uint16_t u; uint16_t g; };
//GB_TAB_BEGIN
static const GbPair GB_TAB[] PROGMEM = {
  {0x00B7,0xA1A4}, {0x00D7,0xA1C1}, {0x2014,0xA1AA}, {0x2026,0xA1AD},  // · × — …
  {0x2192,0xA1FA}, {0x2460,0xA2D9}, {0x2461,0xA2DA}, {0x2605,0xA1EF},  // → ① ② ★
  {0x3000,0xA1A1}, {0x3002,0xA1A3}, {0x3010,0xA1BE}, {0x3011,0xA1BF},  // 　 。 【 】
  {0x4E00,0xD2BB}, {0x4E0A,0xC9CF}, {0x4E0B,0xCFC2}, {0x4E0D,0xB2BB},  // 一 上 下 不
  {0x4E0E,0xD3EB}, {0x4E1C,0xB6AB}, {0x4E24,0xC1BD}, {0x4E2A,0xB8F6},  // 与 东 两 个
  {0x4E2D,0xD6D0}, {0x4E32,0xB4AE}, {0x4E3B,0xD6F7}, {0x4E45,0xBEC3},  // 中 串 主 久
  {0x4E5F,0xD2B2}, {0x4E86,0xC1CB}, {0x4E91,0xD4C6}, {0x4ECE,0xB4D3},  // 也 了 云 从
  {0x4EE4,0xC1EE}, {0x4EF6,0xBCFE}, {0x4EFB,0xC8CE}, {0x4F1A,0xBBE1},  // 令 件 任 会
  {0x4F20,0xB4AB}, {0x4F4D,0xCEBB}, {0x4F4F,0xD7A1}, {0x4F55,0xBACE},  // 传 位 住 何
  {0x4F9B,0xB9A9}, {0x4FDD,0xB1A3}, {0x4FE1,0xD0C5}, {0x4FEE,0xD0DE},  // 供 保 信 修
  {0x505A,0xD7F6}, {0x5165,0xC8EB}, {0x5168,0xC8AB}, {0x516B,0xB0CB},  // 做 入 全 八
  {0x5173,0xB9D8}, {0x5185,0xC4DA}, {0x518C,0xB2E1}, {0x518D,0xD4D9},  // 关 内 册 再
  {0x5199,0xD0B4}, {0x51B2,0xB3E5}, {0x51E0,0xBCB8}, {0x51ED,0xC6BE},  // 写 冲 几 凭
  {0x5207,0xC7D0}, {0x521A,0xB8D5}, {0x521D,0xB3F5}, {0x5230,0xB5BD},  // 切 刚 初 到
  {0x5237,0xCBA2}, {0x524D,0xC7B0}, {0x529F,0xB9A6}, {0x52A1,0xCEF1},  // 刷 前 功 务
  {0x52A8,0xB6AF}, {0x5305,0xB0FC}, {0x5316,0xBBAF}, {0x5323,0xCFBB},  // 动 包 化 匣
  {0x533A,0xC7F8}, {0x5373,0xBCB4}, {0x5386,0xC0FA}, {0x539F,0xD4AD},  // 区 即 历 原
  {0x53CD,0xB7B4}, {0x53D1,0xB7A2}, {0x53D6,0xC8A1}, {0x53D8,0xB1E4},  // 反 发 取 变
  {0x53E3,0xBFDA}, {0x53EA,0xD6BB}, {0x53EF,0xBFC9}, {0x53F2,0xCAB7},  // 口 只 可 史
  {0x53F7,0xBAC5}, {0x5403,0xB3D4}, {0x540C,0xCDAC}, {0x540E,0xBAF3},  // 号 吃 同 后
  {0x5417,0xC2F0}, {0x542F,0xC6F4}, {0x547D,0xC3FC}, {0x54CD,0xCFEC},  // 吗 启 命 响
  {0x5668,0xC6F7}, {0x56DB,0xCBC4}, {0x56DE,0xBBD8}, {0x56E0,0xD2F2},  // 器 四 回 因
  {0x56FA,0xB9CC}, {0x5728,0xD4DA}, {0x5730,0xB5D8}, {0x5740,0xD6B7},  // 固 在 地 址
  {0x5806,0xB6D1}, {0x586B,0xCCEE}, {0x5907,0xB1B8}, {0x590D,0xB8B4},  // 堆 填 备 复
  {0x591A,0xB6E0}, {0x592A,0xCCAB}, {0x5931,0xCAA7}, {0x5934,0xCDB7},  // 多 太 失 头
  {0x59CB,0xCABC}, {0x5B50,0xD7D3}, {0x5B57,0xD7D6}, {0x5B58,0xB4E6},  // 始 子 字 存
  {0x5B8C,0xCDEA}, {0x5B9A,0xB6A8}, {0x5BC6,0xC3DC}, {0x5BF9,0xB6D4},  // 完 定 密 对
  {0x5C06,0xBDAB}, {0x5C0F,0xD0A1}, {0x5C31,0xBECD}, {0x5C4F,0xC6C1},  // 将 小 就 屏
  {0x5D29,0xB1C0}, {0x5DF2,0xD2D1}, {0x5E38,0xB3A3}, {0x5E55,0xC4BB},  // 崩 已 常 幕
  {0x5E72,0xB8C9}, {0x5E76,0xB2A2}, {0x5E94,0xD3A6}, {0x5EFA,0xBDA8},  // 干 并 应 建
  {0x5F00,0xBFAA}, {0x5F0F,0xCABD}, {0x5F53,0xB5B1}, {0x5F55,0xC2BC},  // 开 式 当 录
  {0x5F84,0xBEB6}, {0x5FAA,0xD1AD}, {0x5FD7,0xD6BE}, {0x5FFD,0xBAF6},  // 径 循 志 忽
  {0x6001,0xCCAC}, {0x603B,0xD7DC}, {0x606F,0xCFA2}, {0x60F3,0xCFEB},  // 态 总 息 想
  {0x6210,0xB3C9}, {0x6211,0xCED2}, {0x6216,0xBBF2}, {0x624B,0xCAD6},  // 成 我 或 手
  {0x624D,0xB2C5}, {0x6253,0xB4F2}, {0x626B,0xC9A8}, {0x628A,0xB0D1},  // 才 打 扫 把
  {0x6293,0xD7A5}, {0x62A4,0xBBA4}, {0x62A5,0xB1A8}, {0x62FF,0xC4C3},  // 抓 护 报 拿
  {0x6309,0xB0B4}, {0x636E,0xBEDD}, {0x6389,0xB5F4}, {0x63A5,0xBDD3},  // 按 据 掉 接
  {0x63A9,0xD1DA}, {0x63CF,0xC3E8}, {0x63D0,0xCCE1}, {0x64AD,0xB2A5},  // 掩 描 提 播
  {0x6536,0xCAD5}, {0x6548,0xD0A7}, {0x6570,0xCAFD}, {0x6574,0xD5FB},  // 收 效 数 整
  {0x65B0,0xD0C2}, {0x65B9,0xB7BD}, {0x65E0,0xCEDE}, {0x65E5,0xC8D5},  // 新 方 无 日
  {0x65F6,0xCAB1}, {0x661F,0xD0C7}, {0x662F,0xCAC7}, {0x663E,0xCFD4},  // 时 星 是 显
  {0x6700,0xD7EE}, {0x6709,0xD3D0}, {0x670D,0xB7FE}, {0x672A,0xCEB4},  // 最 有 服 未
  {0x672C,0xB1BE}, {0x673A,0xBBFA}, {0x675C,0xB6C5}, {0x6761,0xCCF5},  // 本 机 杜 条
  {0x6765,0xC0B4}, {0x677F,0xB0E5}, {0x6790,0xCEF6}, {0x679C,0xB9FB},  // 来 板 析 果
  {0x67E5,0xB2E9}, {0x6807,0xB1EA}, {0x6821,0xD0A3}, {0x6837,0xD1F9},  // 查 标 校 样
  {0x683C,0xB8F1}, {0x68C0,0xBCEC}, {0x6A21,0xC4A3}, {0x6B21,0xB4CE},  // 格 检 模 次
  {0x6B63,0xD5FD}, {0x6B65,0xB2BD}, {0x6BB5,0xB6CE}, {0x6C34,0xCBAE},  // 正 步 段 水
  {0x6C42,0xC7F3}, {0x6CA1,0xC3BB}, {0x6CBF,0xD1D8}, {0x6CE2,0xB2A8},  // 求 没 沿 波
  {0x6CE8,0xD7A2}, {0x6D4B,0xB2E2}, {0x6D88,0xCFFB}, {0x6E05,0xC7E5},  // 注 测 消 清
  {0x6E83,0xC0A3}, {0x70B9,0xB5E3}, {0x70ED,0xC8C8}, {0x7247,0xC6AC},  // 溃 点 热 片
  {0x7248,0xB0E6}, {0x7279,0xCCD8}, {0x72B6,0xD7B4}, {0x7387,0xC2CA},  // 版 特 状 率
  {0x73AF,0xBBB7}, {0x751F,0xC9FA}, {0x7528,0xD3C3}, {0x7531,0xD3C9},  // 环 生 用 由
  {0x7535,0xB5E7}, {0x7565,0xC2D4}, {0x7684,0xB5C4}, {0x76D1,0xBCE0},  // 电 略 的 监
  {0x76EE,0xC4BF}, {0x770B,0xBFB4}, {0x77E5,0xD6AA}, {0x77ED,0xB6CC},  // 目 看 知 短
  {0x7801,0xC2EB}, {0x786E,0xC8B7}, {0x793A,0xCABE}, {0x79BB,0xC0EB},  // 码 确 示 离
  {0x79D2,0xC3EB}, {0x7A7A,0xBFD5}, {0x7ACB,0xC1A2}, {0x7B26,0xB7FB},  // 秒 空 立 符
  {0x7B2C,0xB5DA}, {0x7B49,0xB5C8}, {0x7B54,0xB4F0}, {0x7C73,0xC3D7},  // 第 等 答 米
  {0x7CFB,0xCFB5}, {0x7EBF,0xCFDF}, {0x7ED3,0xBDE1}, {0x7EDC,0xC2E7},  // 系 线 结 络
  {0x7EDF,0xCDB3}, {0x7EED,0xD0F8}, {0x7EF4,0xCEAC}, {0x7F13,0xBBBA},  // 统 续 维 缓
  {0x7F51,0xCDF8}, {0x7F6E,0xD6C3}, {0x8054,0xC1AA}, {0x80FD,0xC4DC},  // 网 置 联 能
  {0x8111,0xC4D4}, {0x811A,0xBDC5}, {0x81EA,0xD7D4}, {0x826F,0xC1BC},  // 脑 脚 自 良
  {0x8282,0xBDDA}, {0x82E5,0xC8F4}, {0x83B7,0xBBF1}, {0x85CF,0xB2D8},  // 节 若 获 藏
  {0x884C,0xD0D0}, {0x8868,0xB1ED}, {0x8981,0xD2AA}, {0x89C1,0xBCFB},  // 行 表 要 见
  {0x89E3,0xBDE2}, {0x89E6,0xB4A5}, {0x8BA1,0xBCC6}, {0x8BA4,0xC8CF},  // 解 触 计 认
  {0x8BB0,0xBCC7}, {0x8BBE,0xC9E8}, {0x8BD5,0xCAD4}, {0x8BDD,0xBBB0},  // 记 设 试 话
  {0x8BE2,0xD1AF}, {0x8BF7,0xC7EB}, {0x8BFB,0xB6C1}, {0x8D25,0xB0DC},  // 询 请 读 败
  {0x8D70,0xD7DF}, {0x8D77,0xC6F0}, {0x8DB3,0xD7E3}, {0x8DD1,0xC5DC},  // 走 起 足 跑
  {0x8DDD,0xBEE0}, {0x8DEF,0xC2B7}, {0x8DF3,0xCCF8}, {0x8F6E,0xC2D6},  // 距 路 跳 轮
  {0x8F7D,0xD4D8}, {0x8FB9,0xB1DF}, {0x8FBE,0xB4EF}, {0x8FC7,0xB9FD},  // 载 边 达 过
  {0x8FD0,0xD4CB}, {0x8FD1,0xBDFC}, {0x8FD4,0xB7B5}, {0x8FD8,0xBBB9},  // 运 近 返 还
  {0x8FD9,0xD5E2}, {0x8FDB,0xBDF8}, {0x8FDE,0xC1AC}, {0x9009,0xD1A1},  // 这 进 连 选
  {0x901A,0xCDA8}, {0x90A3,0xC4C7}, {0x90A6,0xB0EE}, {0x90FD,0xB6BC},  // 通 那 邦 都
  {0x914D,0xC5E4}, {0x91CC,0xC0EF}, {0x91CD,0xD6D8}, {0x952E,0xBCFC},  // 配 里 重 键
  {0x957F,0xB3A4}, {0x95EE,0xCECA}, {0x95F4,0xBCE4}, {0x9636,0xBDD7},  // 长 问 间 阶
  {0x963F,0xB0A2}, {0x9664,0xB3FD}, {0x9690,0xD2FE}, {0x9694,0xB8F4},  // 阿 除 隐 隔
  {0x96F6,0xC1E3}, {0x9759,0xBEB2}, {0x975E,0xB7C7}, {0x9762,0xC3E6},  // 零 静 非 面
  {0x9875,0xD2B3}, {0x9898,0xCCE2}, {0x9ED1,0xBADA}, {0x9ED8,0xC4AC},  // 页 题 黑 默
  {0xFF08,0xA3A8}, {0xFF09,0xA3A9}, {0xFF0C,0xA3AC}, {0xFF0F,0xA3AF},  // （ ） ， ／
  {0xFF1A,0xA3BA}, {0xFF1B,0xA3BB}, {0xFF1F,0xA3BF},  // ： ； ？
};
//GB_TAB_END
static uint16_t gbLookup(uint16_t u) {
  for (size_t i = 0; i < sizeof(GB_TAB) / sizeof(GB_TAB[0]); i++) {
    if (pgm_read_word(&GB_TAB[i].u) == u) return pgm_read_word(&GB_TAB[i].g);
  }
  return 0;
}
String gbText(const String &s) {
  String o; o.reserve(s.length() + 16);     // 汉字 3 字节 → 2 字节，只会变小 ✓
  const char *p = s.c_str();
  while (*p) {
    uint8_t c = (uint8_t)*p;
    if (c < 0x80) { o += (char)c; p++; continue; }
    int n; uint16_t u;
    if      ((c & 0xE0) == 0xC0) { u = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { u = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { u = c & 0x07; n = 3; }
    else { p++; o += '?'; continue; }       // 非法首字节：吃掉、给个问号 ✓
    bool ok = true;
    for (int i = 1; i <= n; i++) {
      uint8_t cc = (uint8_t)p[i];
      if (cc == 0 || (cc & 0xC0) != 0x80) { ok = false; break; }   // ★含 0 也要停：绝不读越界 ✓
      u = (uint16_t)((u << 6) | (cc & 0x3F));
    }
    if (!ok) { p++; o += '?'; continue; }
    p += n + 1;
    uint16_t g = gbLookup(u);
    if (g) { o += (char)(g >> 8); o += (char)(g & 0xFF); }
    else   { o += '?'; }
  }
  return o;
}

/* ===== 串口屏发报：**发了就不等**（绝不在主循环里等屏回包 ✗ 会把网页/OTA 卡住 ✓）=====
 * TJC 指令一律以 **3 个 0xFF** 结束 ✓（与收报同一约定 ✓）*/
void tjcSend(const String &cmd) {
  if (!tjcReady) return;                    // 还没 tjc.begin() ⇒ 一个字都不发 ✓
  String out = gbText(cmd);                 // ASCII 原样过、汉字转 GB2312 ✓
  tjc.print(out);
  tjc.write(0xFF); tjc.write(0xFF); tjc.write(0xFF);
}

String tjcEsc(const String &s) {            // TJC 字符串转义： " ⇒ \" ✓；控制字符丢掉 ✓
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    if ((uint8_t)c >= 0x20)    o += c;
  }
  return o;
}
/* ★★ 控件名 / 页名 映射表（2026-09-27 加；同日升级为【按页两级】✓）
 * 起因：编辑器的属性表格【不接受键盘输入】✗ ⇒ 没法把控件改成契约里的名字；
 *       而程序化改 .HMI 又被【页面头 @0 的校验】封死 ✗（改了内容就"加载页面失败"）。
 * 关键实测（决定了本表的结构 ✓）：编辑器的自动命名是【按页独立】的 ✗——
 *       每页的第一个文本都叫 t0 ✓ ⇒ 同一个 t5 在不同页上是不同的控件 ✗✓
 *       ⇒ 所以映射必须带上页名 ✓（两级：{页名, 契约名} → 屏上实际名）
 * 用法：屏上就用编辑器自动名；固件这边翻译 ⇒ 完全不用碰 GUI ✓
 * 维护：①加控件后先跑 verify_hmi.py 读该页的自动名 ②把契约名填进本表 ③再写业务代码
 *       ④将来真把屏上名字改成契约名了，只要把表清空即可（查不到 ⇒ 原样透传 ✓）
 */
static const char *tjcCurPage = "main";      // 当前页（用【契约页名】做键，可读性好 ✓）

struct TjcName { const char *page; const char *want; const char *real; };
static const TjcName TJC_NAME_MAP[] = {
  /* ===== 页 main（屏上 page0）===== */
  { "main", "tTitle", "t0" },
  { "main", "tNet",   "t1" },
  { "main", "tVer",   "t2" },      // ROM 页要的版本号，先借 main 的第三个位（将来 rom 页有自己的 ✓）
  { "main", "tState", "t3" },
  { "main", "tFoot",  "t4" },
  { "main", "tMsg",   "t5" },
  { "main", "tLog",   "t5" },

  /* ===== 页 wifi（屏上 page1）===== */
  { "wifi", "tMsg",    "t10" },    // 通用消息行
  { "wifi", "tSSID",   "t11" },    // 选中/输入的 SSID
  { "wifi", "tPass",   "t12" },    // 密码
  { "wifi", "tKeyNum", "t13" },    // 键盘输入框
  // t0..t9 = 10 行扫描结果（固件按 "t"+row 直接拼名写，见 tjcSet(String("tW")+row,…) 的改造点 ✓）

  /* ===== 页 ip（屏上 page2）===== */
  { "ip", "tIp",   "t0" },
  { "ip", "tMask", "t1" },
  { "ip", "tGw",   "t2" },
  { "ip", "tDns",  "t3" },
  { "ip", "tMsg",  "t4" },

  /* ===== 页 rom（屏上 page3）===== */
  { "rom", "tVer",   "t0" },
  { "rom", "tState", "t1" },
  { "rom", "tLog",   "t2" },
  { "rom", "tMsg",   "t2" },

  /* ===== 页 about（屏上 page4）===== */
  { "about", "tAbout", "t0" },

  /* ===== 页名映射（契约页名 → 屏上数字页）===== */
  { "*", "main",  "0" },
  { "*", "wifi",  "1" },
  { "*", "ip",    "2" },
  { "*", "rom",   "3" },
  { "*", "about", "4" },
};

/* 查表：先按 (当前页, 契约名) 查；再按 (*, 契约名) 查（页名映射与全局项 ✓）；查不到原样返回 ✓ */
static String tjcReal(const String &want) {
  const size_t n = sizeof(TJC_NAME_MAP) / sizeof(TJC_NAME_MAP[0]);
  for (size_t i = 0; i < n; i++) {                       // ① 当前页精确匹配
    if (strcmp(TJC_NAME_MAP[i].page, tjcCurPage) == 0 && want == TJC_NAME_MAP[i].want)
      return String(TJC_NAME_MAP[i].real);
  }
  for (size_t i = 0; i < n; i++) {                       // ② 全局项（"*"）
    if (strcmp(TJC_NAME_MAP[i].page, "*") == 0 && want == TJC_NAME_MAP[i].want)
      return String(TJC_NAME_MAP[i].real);
  }
  return want;                                           // ③ 原样透传 ✓
}

void tjcSet(const String &comp, const String &val) {   // 组件赋值：tNet.txt="..." ✓
  const String real = tjcReal(comp);
  String cmd; cmd.reserve(real.length() + val.length() + 12);
  cmd += real; cmd += F(".txt=\""); cmd += tjcEsc(val); cmd += '"';
  tjcSend(cmd);
}
void tjcPage(const String &pg) {   // 切页：page wifi ⇒ 翻译成数字页，并记住当前页 ✓
  const size_t n = sizeof(TJC_NAME_MAP) / sizeof(TJC_NAME_MAP[0]);
  for (size_t i = 0; i < n; i++) {
    if (strcmp(TJC_NAME_MAP[i].page, "*") == 0 && pg == TJC_NAME_MAP[i].want) {
      /* ★ 只存【表里的字面量指针】⇒ 生命周期安全 ✓（String 的 c_str() 会被临时缓冲坑死 ✗） */
      tjcCurPage = TJC_NAME_MAP[i].want;
      tjcSend(String("page ") + TJC_NAME_MAP[i].real);
      return;
    }
  }
  tjcSend(String("page ") + pg);      // 表里没有 ⇒ 原样发 ✓
}

#else  /* ================= USE_TJC = 0：串口屏整块不编进来，调用点用空桩顶住 ✓ ================= */
/* ★写成和真函数**同签名、非 static**的空实现 —— 
 *   因为 arduino-cli 会自动在上面插原型（原型是无 static 的），带 static 会"冲突声明"报错 ✗ */
String tjcCmd(const String &cmd, uint16_t waitMs) { (void)cmd; (void)waitMs; return String(); }
void   tjcSend(const String &cmd)                  { (void)cmd; }
void   tjcSet(const String &comp, const String &v) { (void)comp; (void)v; }
void   tjcPage(const String &pg)                   { (void)pg; }
#endif /* USE_TJC */

/* 串口屏收报 / 状态推送 / 协议处理定义在后面（这里先声明，避免 .ino 原型生成的顺序问题 ✓）
 * ★2026-09-29（审计建议）：**显式声明别省** ✗ —— 自动生成的函数原型块是"文本级"的，
 *   一旦第一个函数定义落进某个 #if，整块原型就可能被吃掉 ⇒ 调用早于定义的函数直接"未声明"✗。
 *   显式写上这几行，=0/=1 两种配置都不再依赖那个隐式行为 ✓ */
void doAck();
void doSkip();
void tjcPush(bool force);                  // 空桩/真身都在后面，但调用点在前面 ✓
void tjcPoll();
void startAP();                            // ★定义在文件后部（startSTA 里会调它）✓
void startSTA();
/* "本固件没编串口屏"的统一回复 —— ★放无条件区，给 /tjc* 与 /tjcwire 两块共用（审计提醒：别跨 #if 块耦合 ✗）*/
static const char NO_TJC[] = "{\"ok\":false,\"err\":\"本固件编译时未启用串口屏（config.h 里 USE_TJC=0）⇒ 没有屏可测\"}";

String hexOf(const String &s) {
  static const char *H = "0123456789ABCDEF";
  String o; o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == 0xFF) { o += "FF"; }
    else if (c >= 0x20 && c < 0x7F) { o += (char)c; }
    else { o += '['; o += H[c >> 4]; o += H[c & 15]; o += ']'; }
  }
  return o;
}

/* JSON 字符串转义（★/rom 里要塞任意设备文本，比如 API 响应片段里有双引号 ✗
 * 不转义就会把 JSON 打断，浏览器直接 parse 失败 ✗ ⇒ 一律先过这一道 ✓）*/
String jsonEsc(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += F("\\\""); break;
      case '\\': o += F("\\\\"); break;
      case '\n': o += F("\\n");  break;
      case '\r': o += F("\\r");  break;
      case '\t': o += F("\\t");  break;
      default:
        if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04X", (uint8_t)c); o += b; }
        else                   o += c;      // ★>0x7F 的 UTF-8 字节原样过（页面是 UTF-8 ✓）
    }
  }
  return o;
}

/* 下一轮检查要等多少秒（失败时按 60s×失败次数快速重试，上限=正常间隔 ✓）
 * ★loop 与 /rom 必须用**同一个函数**算 ✗→✓：否则页面上写的"下次检查"会跟板子真的对不上 ✓ */
uint32_t waitSecs() {
  uint32_t w = mon.interval;
  if (mon.failCount > 0) {
    w = 60UL * (mon.failCount > 8 ? 8 : mon.failCount);
    if (w > mon.interval) w = mon.interval;
  }
  return w;
}

unsigned long lastScreenTick = 0;           // 屏上时钟刷新用（没校好时时按 60 秒兜底刷 ✓）
time_t        lastShownMin   = 0;           // ★已画在屏上的"第几分钟"（分钟数一变就重画 ✓ 见 loop）
unsigned long lastScreenAt   = 0;           // ★上次重画屏的 millis（/rom 的 scrUp 用来看"多久没刷了"✓）
unsigned long lastNtpTry = 0;               // 校时重试用（30 秒一次 ✓）
unsigned long bootCheckDue = 0;             // 开机那次抓包推迟到什么时候（0 = 不需要 ✓）
bool          apMode     = false;
bool          otaRunning = false;
bool          checking   = false;
bool          checkSoon  = false;           // ★网页【立即检查】置位 ⇒ 主循环里立刻做（不阻塞 HTTP ✓）
unsigned long lastAnnounce = 0, lastBeat = 0, lastPoll = 0, bootMs = 0;
String        lastBody;                    // 最近一次响应（调试用，限长）
String        dlReq = "";                   // ★"请求下载"标志：按住按键 3 秒 或 网页按钮置位，电脑端来取 ✓（只放 RAM ✓）

String chipHex() { char b[16]; snprintf(b, sizeof(b), "%06X", ESP.getChipId()); return String(b); }
String hostName() { return String(FW_NAME) + "-" + chipHex(); }

void ledOn()  { digitalWrite(LED_PIN, LOW); }
void ledOff() { digitalWrite(LED_PIN, HIGH); }
/* ★2026-09-29（主人报「有新版本怎么不会亮灯」）：
 *   灯这一路以前是**哑的** —— 出问题只能猜 ✗。现在补三样，让"灯到底想不想亮"可以远程量：
 *   ① `/led?on=1|0|2`  手动点灯（强制 15 秒，期间主循环不抢 ✓）⇒ 先证明"灯和 IO 是好的" ✓
 *   ② `/rom` 报 `led`（**直接回读引脚电平**）+ `otaRunning`（OTA 期间主循环不碰灯 ✓）
 *   ③ 网页「设备状态」里显示"报警灯 亮/灭" ✓ */
unsigned long ledForceUntil = 0;           // 手动点灯的强制窗口（毫秒时间戳，0=没强制）

bool hasNew() {
  if (mon.latest[0] == 0) return false;
  if (strcmp(mon.latest, mon.ack) == 0) return false;
  if (mon.skip[0] && strcmp(mon.latest, mon.skip) == 0) return false;
  return true;
}
/* ★ledAlarm() 定义在 Ext 结构之后（它要用 ext.seen ✓）—— 
 *   顺手记个教训：**在 .ino 里加函数别忘了"它依赖的结构体声明在哪"** ✗（本喵这次就放早了、编译报 'ext' 未声明 ✓）*/

// ---------------------------------------------------------------- EEPROM
void saveCfg(const String &ssid, const String &pass) {
  cfg.magic = CFG_MAGIC;
  memset(cfg.ssid, 0, sizeof(cfg.ssid));
  memset(cfg.pass, 0, sizeof(cfg.pass));
  strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
  strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
  EEPROM.put(CFG_OFF, cfg);
  EEPROM.commit();
}
void clearCfg() { cfg.magic = 0; cfg.ssid[0] = 0; EEPROM.put(CFG_OFF, cfg); EEPROM.commit(); }

void monSave() { EEPROM.put(MON_OFF, mon); EEPROM.commit(); }

void monInit() {
  EEPROM.get(MON_OFF, mon);
  if (mon.magic != MON_MAGIC) {
    memset(&mon, 0, sizeof(mon));
    mon.magic = MON_MAGIC;
    mon.interval = DFLT_INTERVAL;
    strncpy(mon.host, DFLT_HOST, sizeof(mon.host) - 1);
    strncpy(mon.path, DFLT_PATH, sizeof(mon.path) - 1);
    monSave();
  } else {
    if (mon.interval < 60) mon.interval = DFLT_INTERVAL;
    if (mon.host[0] == 0) strncpy(mon.host, DFLT_HOST, sizeof(mon.host) - 1);
    if (mon.path[0] == 0) strncpy(mon.path, DFLT_PATH, sizeof(mon.path) - 1);
  }
}

/* ===== 串口屏用的网络配置（静态 IP / DHCP）=====
 * ★★EEPROM 布局是红线（主人明说）：Cfg 在 0（与 ota-base 共用，一个字节都不能动 ✗）、Mon 在 128、
 *   Hist 在 512、EEPROM.begin(1024)。所以新东西**一律放 768 起、自带独立 magic** ✓
 *   （1KB 的尾巴还剩 768~1023 共 256 字节，本结构只占 24 ✓）。
 * ★结构一变就**换 magic 值** ⇒ 老数据自动作废，绝不会把旧字节当成有效配置读进来 ✓ */
#define NET_MAGIC 0x4E455432UL             // "NET2"（结构改了就把 2 往上加 ✓）
#define NET_OFF   768
struct NetCfg {
  uint32_t magic;
  uint8_t  useStatic;                      // 0 = DHCP（默认）；1 = 静态
  uint8_t  rsv[3];                         // 对齐用（现在是 24 字节 ✓）
  uint32_t ip, mask, gw, dns;              // 主机序 IPv4（与 IPAddress 的 uint32 表示一致 ✓）
} net;
void netSave() { EEPROM.put(NET_OFF, net); EEPROM.commit(); }
void netLoad() {
  EEPROM.get(NET_OFF, net);
  if (net.magic != NET_MAGIC) {            // 新芯片 / 换过 magic ⇒ 归零当 DHCP ✓
    memset(&net, 0, sizeof(net));
    net.magic = NET_MAGIC;
    netSave();
  }
}

/* ===== 通用化：数据源预设 + 页面标题（★2026-09-29 加）=========================
 * 为什么要有它：以前抓取逻辑**写死**成 "OpenList 目录 API 专用"（固定 POST /api/fs/list、
 *   固定扫 "name"/"created"）⇒ 想监测别的（比如 GitHub Release）就得改代码 ✗。
 * 现在：把"请求方式 + 取值键 + 时间键 + 人看的网页基址 + 标题"全做成配置 ⇒
 *   **同一套扫描器**既能读 OpenList 目录，也能读 GitHub Release，甚至任意 JSON 接口 ✓
 * ★放哪：EEPROM **800 起**（Cfg 0 / Mon 128 / Hist 512 / Net 768 一个字节都不动 ✓）；
 *   自带独立 magic ⇒ 以后结构再变就 +1，老数据自动作废 ✓
 * ★实测依据（2026-09-29 真实抓包核实，不是猜的）：
 *   · GitHub：`tag_name` + `published_at`（定长 UTC，字典序=时间序 ✓）、必须带 User-Agent、
 *     未鉴权限流 60 次/小时/IP ⇒ 轮询别低于 10 分钟 ✓
 *   · OpenList：`name` + `created`（原行为不变 ✓） */
#define EXT_MAGIC 0x45585434UL             // "EXT4"（2026-09-29：加了 seen（"我知道了"）、body 缩到 32 ✓）
#define EXT_OFF   800
#define PRESET_OPENLIST 0
#define PRESET_GITHUB   1
#define PRESET_CUSTOM   2
#define M_GET  0
#define M_POST 1
struct Ext {
  uint32_t magic;
  uint8_t  preset;                         // 0=OpenList 目录 1=GitHub Release 2=自定义
  uint8_t  method;                         // 0=GET 1=POST
  uint8_t  rsv[2];
  char     jsonKey[16];                    // 取值键： "name" / "tag_name"
  char     timeKey[16];                    // 时间键： "created" / "published_at"
  char     title[24];                      // 页面/OLED/串口屏上的标题（默认"系统包监测"）
  char     webBase[64];                    // 人看的网页基址（点一下能打开；空=用 host+path 拼）
  char     body[32];                       // POST 体（空 = 按 preset 自动生成 ✓；自定义 POST 才用得上）
  /* ★2026-09-29 加：**"我知道了"（关灯但不改语义）** —— 
   *   起因：主人原话「应该设计一个我已知道然后不闪灯了的方法」✓ ——
   *   原来只有"我已刷"（＝我刷了 ✓）和"跳过这版"（＝我不要这版 ✓），**都带语义** ✗；
   *   而"我看到提醒了、先把灯摁灭"是第三种意思 ✓ ⇒ 单独存一个 `seen`：
   *     · 页面照样显示"有新版本" ✓（信息不丢）
   *     · **灯只用 "还有没有没看过的版本" 判断** ⇒ 摁灭后不再亮；**真出新版本又会亮** ✓✓ */
  char     seen[24];                      // 已经"知道过了"的版本（＝灯可以灭的那个版本）
  /* ★2026-09-29 加：**当前这个"最新版"是（本地时间）什么时候第一次被看到的** —— 
   *   起因：主人问「11 点传的为什么 4 点才抓到」✗，而**历史表只留 6 条≈50 分钟**⇒ 根本答不上来 ✗✓。
   *   现在把"首次发现时刻"单独存下来 ⇒ 页面能直接写"新版本 vX 首次发现 2026-09-29 11:24（已提醒 5 小时）"✓✓ */
  char     found[32];                      // 首次发现时刻的人话字符串（没校时则写"开机 N 秒"）
  uint32_t foundUp;                        // 首次发现时的 uptime 秒（用来算"已提醒多久"，不依赖时钟 ✓）
} ext;

void extSave() { EEPROM.put(EXT_OFF, ext); EEPROM.commit(); }

/* 把某个预设的默认值填进 ext（网页上点一下预设就走这里 ✓） */
void extApplyPreset(uint8_t p) {
  ext.preset = p;
  switch (p) {
    case PRESET_GITHUB:
      ext.method = M_GET;
      strncpy(ext.jsonKey, "tag_name",     sizeof(ext.jsonKey) - 1);
      strncpy(ext.timeKey, "published_at", sizeof(ext.timeKey) - 1);
      ext.body[0] = 0;                     // GitHub 不需要请求体 ✓
      break;
    case PRESET_CUSTOM:
      if (ext.method != M_GET && ext.method != M_POST) ext.method = M_GET;
      break;
    case PRESET_OPENLIST:
    default:
      ext.preset = PRESET_OPENLIST;
      ext.method = M_POST;
      strncpy(ext.jsonKey, "name",    sizeof(ext.jsonKey) - 1);
      strncpy(ext.timeKey, "created", sizeof(ext.timeKey) - 1);
      ext.body[0] = 0;                     // 空 = 抓取时按 mon.path 自动拼 ✓
      break;
  }
  ext.jsonKey[sizeof(ext.jsonKey) - 1] = 0;
  ext.timeKey[sizeof(ext.timeKey) - 1] = 0;
}

void extLoad() {
  EEPROM.get(EXT_OFF, ext);
  bool bad = (ext.magic != EXT_MAGIC) ||
             ext.jsonKey[0] == 0 || ext.timeKey[0] == 0 ||
             (ext.method != M_GET && ext.method != M_POST);
  if (bad) {                               // 新芯片 / 结构变过 / 字段坏 ⇒ 回默认 OpenList ✓
    memset(&ext, 0, sizeof(ext));
    ext.magic = EXT_MAGIC;
    extApplyPreset(PRESET_OPENLIST);
    strncpy(ext.title, DFLT_TITLE, sizeof(ext.title) - 1);   // ★可配（见适配层 DFLT_TITLE ✓；字模要有这几个字 ✓）
    extSave();
    Serial.println("[EXT] 新建数据源配置（默认 OpenList 预设 ✓）");
  }
  if (ext.preset > PRESET_CUSTOM) ext.preset = PRESET_OPENLIST;
  if (ext.title[0] == 0) strncpy(ext.title, DFLT_TITLE, sizeof(ext.title) - 1);
}

/* ★2026-09-29：**灯与页面分开判断** ——
 *   页面用 hasNew()（有新版本就一直显示 ✓ 信息不丢）；
 *   灯用 ledAlarm()（＝"还有没看过的版本"⇒ 主人摁过"我知道了"就灭 ✓，真出新版本又会亮 ✓✓）*/
bool ledAlarm() {
  if (!hasNew()) return false;
  if (ext.seen[0] && strcmp(mon.latest, ext.seen) == 0) return false;
  return true;
}

/* 数据源一句话描述（给网页/日志看 ✓） */String extPresetName() {
  switch (ext.preset) {
    case PRESET_GITHUB: return "GitHub Release";
    case PRESET_CUSTOM: return "自定义 JSON";
    default:            return "OpenList 目录";
  }
}
/* "点一下就能打开"的网页地址：配了 webBase 就用它，否则按 host+path 拼 ✓ */
String extWebUrl(const char* ver) {
  String u;
  if (ext.webBase[0]) {
    u = String(ext.webBase);
    if (ver && ver[0]) { if (!u.endsWith("/")) u += "/"; u += ver; }
  } else {
    u = String("https://") + String(mon.host) + String(mon.path);
    if (ver && ver[0]) u += String("/") + ver;
  }
  return u;
}

/* 解析 "a.b.c.d"：正好四段、每段 0~255，多一个字符都不认 ✓（不依赖库里的宽松解析 ✓） */
bool parseIp(const String &s, IPAddress &out) {
  String t = s; t.trim();
  uint8_t b[4]; int part = 0, v = 0, digits = 0;
  for (unsigned int i = 0; i <= t.length(); i++) {
    char c = (i < t.length()) ? t.charAt(i) : '.';   // 末尾补一个 '.' 把最后一段冲出来 ✓
    if (c >= '0' && c <= '9') {
      v = v * 10 + (c - '0');
      if (++digits > 3 || v > 255) return false;
    } else if (c == '.') {
      if (!digits || part > 3) return false;
      b[part++] = (uint8_t)v; v = 0; digits = 0;
    } else {
      return false;
    }
  }
  if (part != 4) return false;
  out = IPAddress(b[0], b[1], b[2], b[3]);
  return true;
}

/* 联网前应用静态 IP：★必须在 WiFi.begin() **之前**调 ✓；是 DHCP 就**不调** ✓ */
void applyStaticIp() {
  if (net.magic != NET_MAGIC || net.useStatic != 1) return;
  if (!net.ip || !net.mask) {               // 数据不全 ⇒ 当 DHCP（宁可没静态 IP，也不能没网 ✗）
    Serial.println("[NET] 静态 IP 数据不全 ⇒ 按 DHCP 走");
    return;
  }
  IPAddress ip(net.ip), mask(net.mask), gw(net.gw), dns(net.dns);
  if (!gw)  gw  = IPAddress(ip[0], ip[1], ip[2], 1);   // 没写网关 ⇒ 猜 .1 ✓
  if (!dns) dns = gw;
  if (WiFi.config(ip, gw, mask, dns)) {
    Serial.printf("[NET] 静态 IP %s/%s 网关 %s DNS %s\n",
                  ip.toString().c_str(), mask.toString().c_str(),
                  gw.toString().c_str(), dns.toString().c_str());
  } else {
    Serial.println("[NET] WiFi.config 失败 ✗ ⇒ 这轮可能拿不到地址");
  }
}

// ---------------------------------------------------------------- HTTP(S) 抓取
// 手写 chunked 解码 + 极简 JSON 扫描：找出上传时间最大的 "name"
bool fetchLatest(String &outName, String &outTime, String &err) {
  /* ★2026-09-30（适配/开源）：默认数据源现在是**空的**（中性默认值 ✓）⇒
   *   没配就直接给一句人话，别拿着空主机名去 TLS 握手 ✗（那会报一堆看不懂的 SSL 错误 ✓）*/
  if (!mon.host[0]) {
    err = "还没配数据源：网页「监测设置」里填服务器与目录 ✓";
    return false;
  }
  uint32_t heap0 = ESP.getFreeHeap();
  if (heap0 < 14000) {                     // ★堆不够就先不抓（宁可这轮跳过，也不要把板子搞崩 ✗）
    err = "堆不足，跳过本轮: " + String(heap0);
    return false;
  }
  /* ★★2026-09-26 真凶修复（黑匣子实测：阶段 11 崩溃，原因 "heap 24144->1968" ✗）：
   * BearSSL 默认给 **16KB 接收缓冲** ⇒ 板子本来只有 ~24KB 堆 ⇒ 握手时直接见底 ⇒ **异常重启** ✗✗
   * （表现就是主人说的「自检有时候过不去、好几次才进去」✓ —— 屏幕反复回到自检页 ✓）。
   * 本代码是**逐字节读**响应 ✓，根本不需要大缓冲 ⇒ 收到 4KB 足够 ✓，每次省下 ~12KB ✓✓ */
  tls.setBufferSizes(4096, 512);
  tls.setInsecure();                       // 公开列表，先不校验证书；要严可换 setTrustAnchors
  tls.setTimeout(15);

  if (!tls.connect(mon.host, 443)) {
    char eb[80] = {0};
    tls.getLastSSLError(eb, sizeof(eb));   // BearSSL 的真实原因（OOM / 握手失败…）
    err = String("TLS失败: ") + (eb[0] ? eb : "?") +
          " heap " + String(heap0) + "->" + String(ESP.getFreeHeap());
    tls.stop();                            // ★ 失败也必须 stop，否则 BearSSL 缓冲泄漏
    return false;
  }

  String body;                             // ★ 连上之后再要内存：握手时给 TLS 留干净的堆
  body.reserve(4608);

  /* ★2026-09-29 通用化：请求行/请求体按 ext 配置现场生成 —— 同一套代码既能打 OpenList 也能打 GitHub ✓
   *   · OpenList（preset 0）：POST /api/fs/list + {"path":…}
   *   · GitHub  （preset 1）：GET  <ext.path>（例 /repos/OWNER/REPO/releases/latest），无请求体
   * ★实测结论（2026-09-29 抓包核实，不是猜的）：
   *   · GitHub **必须带 User-Agent**，不带直接 403（体："Request forbidden by administrative rules"）
   *   · "Accept-Encoding: identity" ＝"不要压缩" ✓ —— 千万别请求 gzip，板子解不了 ✓
   *   · 未鉴权限流 60 次/小时/IP ⇒ 轮询别低于 10 分钟（默认 600 秒 ✓ 安全）*/
  String payload;
  if (ext.method == M_POST) {
    payload = ext.body[0] ? String(ext.body)
                          : String("{\"path\":\"") + mon.path + "\",\"password\":\"\",\"page\":1,\"per_page\":0,\"refresh\":false}";
  }
  /* ★★2026-09-29 血泪 bug（真机测出来的，编译期完全看不出来 ✗）：
   *   "请求路径"和"要盯的目标"是**两件事**，别混 ✓
   *   · OpenList：请求路径**恒定**是 `/api/fs/list`；mon.path（要看的目录）走 **POST body** ✓
   *     ——本喵一开始直接把 mon.path 当请求路径 ⇒ POST 打到目录上 ⇒ 服务器回**网页 HTML** ⇒
   *       报 "API 非 200" ✗（现象很像"服务器挂了"，其实是自己打歪了 ✓）
   *   · GitHub / 自定义：mon.path **本身就是请求路径**（/repos/O/R/releases/latest）✓ */
  String reqPath = (ext.preset == PRESET_OPENLIST) ? String("/api/fs/list") : String(mon.path);
  String req = String(ext.method == M_POST ? "POST " : "GET ") + reqPath + " HTTP/1.1\r\n";
  req += "Host: " + String(mon.host) + "\r\n";
  req += "User-Agent: " + hostName() + "/" + FW_VERSION + "\r\n";   // ★GitHub 必带（实测缺它 403 ✓）
  req += "Accept: application/json\r\n";
  req += "Accept-Encoding: identity\r\n";                          // identity＝不压缩（主板只认这个 ✓）
  if (ext.method == M_POST) {
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + String(payload.length()) + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  tls.print(req);
  if (payload.length()) tls.print(payload);

  // 1) 读头
  String header; unsigned long t0 = millis();
  while (millis() - t0 < 20000) {
    if (!tls.available()) { if (!tls.connected()) break; delay(5); continue; }
    char c = tls.read();
    header += c;
    if (header.endsWith("\r\n\r\n")) break;
    if (header.length() > 2048) break;
  }
  if (!header.startsWith("HTTP/1.")) { err = "无 HTTP 响应"; tls.stop(); return false; }
  int sp = header.indexOf(' ');
  int code = header.substring(sp + 1, sp + 4).toInt();
  bool chunked = header.indexOf("chunked") >= 0;

  // 2) 读体（de-chunk）
  t0 = millis();
  if (chunked) {
    while (millis() - t0 < 25000) {
      String line = tls.readStringUntil('\n'); line.trim();
      if (line.length() == 0) continue;
      long sz = strtol(line.c_str(), nullptr, 16);
      if (sz <= 0) break;
      for (long i = 0; i < sz && body.length() < 12000; i++) {
        if (!tls.available()) { if (!tls.connected()) break; delay(2); }
        body += (char)tls.read();
      }
      tls.readStringUntil('\n');            // 尾随 CRLF
    }
  } else {
    while (millis() - t0 < 25000 && body.length() < 12000) {
      if (!tls.available()) { if (!tls.connected()) break; delay(5); continue; }
      body += (char)tls.read();
    }
  }
  tls.stop();
  lastBody = body.substring(0, 400);

  if (code != 200) {
    /* ★通用化后这里要分清两种情况：①HTTP 不是 200（GitHub 的 403/404 都落这里）
     *   ②OpenList 特有的"业务码"（HTTP 200 但 body 里 code≠200）⇒ 后者只在该预设下判 ✓ */
    if (ext.preset == PRESET_OPENLIST && code == 200) {
      int m = body.indexOf("\"message\":\"");
      err = (m > 0) ? body.substring(m + 11, body.indexOf('"', m + 11)) : "API 非 200";
    } else {
      err = "HTTP " + String(code);
      if (code == 403) err += "（限流？GitHub 未鉴权 60 次/小时 ⇒ 把间隔调大 ✓ 或 User-Agent 被拦）";
      if (code == 404) err += "（仓库/路径不存在，或该仓库没有已发布的 release ✓）";
    }
    return false;
  }
  if (ext.preset == PRESET_OPENLIST && body.indexOf("\"code\":200") < 0) {
    int m = body.indexOf("\"message\":\"");
    err = (m > 0) ? body.substring(m + 11, body.indexOf('"', m + 11)) : "API 非 200";
    return false;
  }

  /* 3) 扫描：找 "<jsonKey>":"X" → 取其后第一个 "<timeKey>":"T" → 留 T 最大的那条
   *    OpenList = name/created ；GitHub = tag_name/published_at ⇒ **同一套逻辑** ✓
   *    ★键用【带引号带冒号】的完整形式：既避免撞上 assets 段里的 "name":" ✓
   *      （实测：GitHub 正文里 "name": 出现在 assets 里、tag_name 唯一 ✓）
   *    ★值为 null 的条目不会匹配（`"tag_name":null` 没有引号）⇒ 自动跳过 ✓ */
  const String kName = String("\"") + ext.jsonKey + "\":\"";
  const String kTime = String("\"") + ext.timeKey + "\":\"";
  outName = ""; outTime = "";
  int idx = 0;
  while (true) {
    int n = body.indexOf(kName, idx);
    if (n < 0) break;
    int nEnd = body.indexOf('"', n + kName.length());
    if (nEnd < 0) break;
    String name = body.substring(n + kName.length(), nEnd);
    int c = body.indexOf(kTime, nEnd);
    int nNext = body.indexOf(kName, nEnd);
    if (c > 0 && (nNext < 0 || c < nNext)) {
      int cEnd = body.indexOf('"', c + kTime.length());
      if (cEnd > c) {
        String when = body.substring(c + kTime.length(), cEnd);
        if (when.length() >= 19) when = when.substring(0, 19);   // 2026-09-23T10:38:52
        /* ★字典序＝时间序：OpenList 与 GitHub 都是定长 UTC ISO（…Z）⇒ 直接字符串比大小 ✓ */
        if (name.length() && (outTime.length() == 0 || when > outTime)) { outTime = when; outName = name; }
      }
    }
    idx = nEnd;
  }
  if (outName.length() == 0) {
    err = String("没解析到条目（正文里没找到键 \"") + ext.jsonKey + "\"？检查预设/路径 ✓）";
    return false;
  }
  return true;
}

#if USE_OLED
/* ===== 中文点阵渲染（16x16 汉字 + 内置 ASCII 混排） ===== */
static const uint8_t* cnLookup(uint16_t code) {
  for (int i = 0; i < CN_GLYPHS_COUNT; i++) {
    if (pgm_read_word(&CN_GLYPHS[i].code) == code) return CN_GLYPHS[i].bmp;
  }
  return nullptr;
}

static void cnDrawGlyph(int x, int y, const uint8_t* bmp) {
  for (int row = 0; row < 16; row++) {
    for (int col = 0; col < 16; col++) {
      uint8_t b = pgm_read_byte(bmp + row * 2 + (col >> 3));
      if (b & (0x80 >> (col & 7))) oled.setPixel(x + col, y + row);
    }
  }
}

/* ★2026-09-30：ASCII 的**步进用库里的真实字宽**（原来写死 9px ✗）——
 *   库里 ArialMT_Plain_16 的 'O' 是 12px 宽、'K' 11px ⇒ 步进 9px 会让 O 和 K **叠 3px** ✗，
 *   自检页的「按键 OK / 总线 OK」就会糊成一坨（这正是主人说的"右边看着不对"的一类 ✗）。
 *   现在：setFont 后问库要真实宽度 +1px 间距 ⇒ 不叠、不超右边界 ✓（不占 PROGMEM ✓）*/
static int uiAsciiW(char c, bool big24) {
  char one[2] = { c, 0 };
  oled.setFont(big24 ? ArialMT_Plain_24 : ArialMT_Plain_16);
  return (int)oled.getStringWidth(one, 1, false) + 1;
}

/* 画一行中英混排：ASCII 用内置 16px 字体（按真实字宽步进），汉字 16px 宽 ✓ */
static void uiText(int x, int y, const char* str) {
  const unsigned char* p0 = (const unsigned char*)str;
  while (*p0) {
    if (*p0 < 0x80) {
      char one[2] = { (char)*p0, 0 };
      oled.setFont(ArialMT_Plain_16);
      oled.drawString(x, y, one);
      x += uiAsciiW((char)*p0, false);
      p0++;
    } else {
      uint16_t code = ((uint16_t)(p0[0] & 0x0F) << 12) | ((uint16_t)(p0[1] & 0x3F) << 6) | (uint16_t)(p0[2] & 0x3F);
      const uint8_t* g = cnLookup(code);
      if (g) cnDrawGlyph(x, y, g);
      else   oled.drawRect(x, y, 15, 15);   // 缺字：画空框，提醒补字模
      x += 16;
      p0 += 3;
    }
  }
}

/* ★2026-09-30 修（自检页两列**压字**✗）：
 *   老写法「显示屏 OK」整串 16px 有 **78px 宽** ✗，而右列是从 x=64 起画的 ⇒ 
 *   左列的 "OK" 直接**糊在右列「按键」头上** ✗（开机自检页看着就是一团）。
 *   现在：项目名照旧 16px，**状态标记 OK/-- 改用 10px Arial**（整串只有 15px ✓）⇒
 *   左列最宽到 63、右列 72~119 —— 两列彻底分开，也碰不到右边界 ✓✓ */
/* 一行中英混排按 16px 渲染会有多宽（汉字 16 + ASCII 真实字宽 ✓）—— 用来**先算再画**，
 * 避免"放不下还硬画 ⇒ 撞到别的字/超出右边界" ✗（128x32 那种小屏全靠它判断 ✓）*/
static int uiStrW16(const char* s) {
  int w = 0;
  const unsigned char* p = (const unsigned char*)s;
  while (*p) { if (*p < 0x80) { w += uiAsciiW((char)*p, false); p++; } else { w += 16; p += 3; } }
  return w;
}

static void uiItem(int x, int y, const char* name, bool ok) {
  uiText(x, y, name);
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(x + uiStrW16(name) + 1, y + 3, ok ? "OK" : "--");
}

/* ===== 24x24 汉字渲染（标题专用：宋体 24px 点阵笔画更全 ✓） ===== */
static const uint8_t* cnLookup24(uint16_t code) {
  for (int i = 0; i < CN_GLYPHS24_COUNT; i++) {
    if (pgm_read_word(&CN_GLYPHS24[i].code) == code) return CN_GLYPHS24[i].bmp;
  }
  return nullptr;
}

static void cnDrawGlyph24(int x, int y, const uint8_t* bmp) {
  for (int row = 0; row < 24; row++) {
    for (int col = 0; col < 24; col++) {
      uint8_t b = pgm_read_byte(bmp + row * 3 + (col >> 3));
      if (b & (0x80 >> (col & 7))) oled.setPixel(x + col, y + row);
    }
  }
}

static void uiText24(int x, int y, const char* str) {   // 中英混排 24px（ASCII 按真实字宽步进，汉字 24px）
  const unsigned char* p0 = (const unsigned char*)str;
  while (*p0) {
    if (*p0 < 0x80) {
      char one[2] = { (char)*p0, 0 };
      oled.setFont(ArialMT_Plain_24);
      oled.drawString(x, y, one);
      x += uiAsciiW((char)*p0, true);
      p0++;
    } else {
      uint16_t code = ((uint16_t)(p0[0] & 0x0F) << 12) | ((uint16_t)(p0[1] & 0x3F) << 6) | (uint16_t)(p0[2] & 0x3F);
      const uint8_t* g = cnLookup24(code);
      if (g) cnDrawGlyph24(x, y, g);
      else   oled.drawRect(x, y, 23, 23);
      x += 24;
      p0 += 3;
    }
  }
}
#endif /* USE_OLED —— 以上是 16/24px 中文点阵渲染（只有 OLED 在用）✓ */

/* ===== 开机自检：逐项显示，坏哪一项一眼看得出 ✓ ===== */
/* ===== 启动黑匣子（放 RTC 用户内存：**异常重启也不丢** ✓✓）=====
 * 主人 2026-09-26 原话：「自检有时候过不去，多次之后才进去…你有写日志吗」✗
 * ⇒ 之前只打在串口上（主人看不到 ✗）。现在记：启动次数 / 上次复位原因 / **崩在第几阶段（面包屑 ✓）** /
 *   上次自检五项结果 / WiFi 连了多久 / 上次检查失败原因 ✓  —— 全部可从 /log 与 /status 远程读 ✓ */
#define BB_MAGIC   0x424F5804UL
#define BB_RTC_OFF 0                     // RTC 用户内存偏移（单位：4 字节字 ✓）
#define PH_SETUP   1
#define PH_WIRE    2
#define PH_SELFTEST 3
#define PH_EEPROM  4
#define PH_WIFI    5              // 连 WiFi
#define PH_NTP     6              // ★NTP 校时（原来和连网混在一起 ✗ ⇒ 崩了说不清 ✓ 现在单独一段 ✓）
#define PH_SERVER  7              // 注册路由/开服务器
#define PH_OTA     9              // ArduinoOTA.begin()（会起 mDNS ⇒ 吃内存 ✗ 重点怀疑 ✓）
#define PH_DEV    10              // UDP 播报 / 串口屏初始化
#define PH_CHECK  11              // ★开机第一次抓包（TLS 握手，堆最紧的时候 ✗ 重点怀疑 ✓）
#define PH_LOOP    8              // 主循环
struct BlackBox {
  uint32_t magic;
  uint32_t boots;            // 累计启动次数（主人说"好几次才进去" ⇒ 这个数会一路涨 ✓）
  uint32_t lastResetMs;      // 上次跑到"主循环"用了多久（毫秒）
  uint8_t  phaseLast;        // ★"上一次"跑到哪个阶段就崩了（开机时**保留** ✓，绝不覆盖 ✗）
  uint8_t  phase;            // 本次跑到哪个阶段（每次开机清零 ✓）
  uint8_t  oled, i2c, btn, cfg, net;   // 上次自检五项 ✓
  uint16_t wifiMs;           // 连 WiFi 花了多久（0 = 没连上 ✓）
  char     resetReason[28];  // 上次复位原因（如 Exception / Software Watchdog ✓）
} bb;

static void bbPhase(uint8_t ph) { bb.phase = ph; ESP.rtcUserMemoryWrite(BB_RTC_OFF, (uint32_t*)&bb, sizeof(bb)); }
static void bbSave()            { ESP.rtcUserMemoryWrite(BB_RTC_OFF, (uint32_t*)&bb, sizeof(bb)); }
static void bbLoad() {
  ESP.rtcUserMemoryRead(BB_RTC_OFF, (uint32_t*)&bb, sizeof(bb));
  if (bb.magic != BB_MAGIC) { memset(&bb, 0, sizeof(bb)); bb.magic = BB_MAGIC; }
  bb.boots++;
  String rr = ESP.getResetReason();
  if (rr.length() == 0) rr = "?";
  rr.toCharArray(bb.resetReason, sizeof(bb.resetReason));
  bb.resetReason[sizeof(bb.resetReason) - 1] = 0;
  bb.phaseLast = bb.phase;            // ★先把"上次崩在哪"留档 ✓（这是黑匣子的命根子 ✓）
  bb.phase = 0;                       // 本次的面包屑从头开始 ✓
  bbSave();
}
static const char* phName(uint8_t ph) {
  switch (ph) {
    case 0:           return "刚升级/首次上电（RTC 记忆被清）";   // ★OTA 会清 RTC ⇒ 别当成"崩了" ✗
    case PH_SETUP:    return "setup开头";
    case PH_WIRE:     return "OLED/总线初始化";
    case PH_SELFTEST: return "开机自检";
    case PH_EEPROM:   return "读EEPROM配置";
    case PH_WIFI:     return "连WiFi";
    case PH_NTP:      return "NTP校时";
    case PH_SERVER:   return "注册路由/开服务器";
    case PH_OTA:      return "ArduinoOTA.begin(起mDNS)";
    case PH_DEV:      return "UDP播报/串口屏初始化";
    case PH_CHECK:    return "开机第一次抓包(TLS)";
    case PH_LOOP:     return "主循环";
    default:          return "（没到达任何阶段）";
  }
}

static bool stOled = false, stI2c = false, stBtn = false, stCfg = false, stNet = false;

#if USE_OLED
static void stDraw() {
  oled.clear();
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
#if OLED_SMALL
  /* ===== 128x32 自检页（只有两行，5 项塞不下 ⇒ 只列**没过的**那几项 ✓）=====
   * ★2026-09-30 适配：原版面按 64 行写死 ✗ ⇒ 32 行屏上第 2 行以后全画到屏幕外 ✓ */
  uiText(0, 0, "系统自检");
  String bad = "";
  if (!stOled) bad += "屏";
  if (!stI2c)  bad += "线";
  if (!stBtn)  bad += "键";
  if (!stCfg)  bad += "配";
  if (!(stNet || WiFi.status() == WL_CONNECTED)) bad += "网";
  if (bad.length() == 0) uiText(0, 16, "全部正常");
  else {
    oled.setFont(ArialMT_Plain_10);
    oled.drawString(0, 20, "NG:");          // 10px 的 "NG:" 约 18px 宽 ⇒ 从 24 起画汉字不撞 ✓
    uiText(24, 16, bad.c_str());
  }
#else
  uiText(0, 0, "系统自检");
  oled.drawLine(0, 15, 127, 15);   // 分隔线上移 1px，别压住下面那行字 ✓
  uiItem(0, 16, "显示屏", stOled);
  uiItem(0, 32, "配置",   stCfg);
  uiItem(72, 16, "按键",  stBtn);
  uiItem(72, 32, "总线",  stI2c);
  if (stNet || WiFi.status() == WL_CONNECTED) uiItem(0, 48, "网络", true);
  else                                        uiText(0, 48, "网络 连接中");
#endif
  oled.display();
}

void bootSelfTest() {                      // 在 setup 里、连 WiFi 之前调用
  stOled = true;                           // 能画出来就说明屏没问题 ✓
  stCfg  = (mon.magic == MON_MAGIC);
  stI2c  = false;
  for (uint8_t a = 0x03; a < 0x78; a++) {  // 扫总线确认 OLED 在
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && a == 0x3C) { stI2c = true; break; }
  }
  bbPhase(PH_WIRE);
  pinMode(BTN_PIN, INPUT_PULLUP);
  stBtn = (digitalRead(BTN_PIN) == HIGH);  // 没被按住算正常
  stNet = (WiFi.status() == WL_CONNECTED);
  stDraw();
  delay(700);
}

void oledBanner(const char* msg) {
  /* ★2026-09-25 修（主人报「点一下确认键那个数字会消失」✗）：
   * 旧写法有两个病：①用内置 ArialMT_Plain_16 画汉字 ✗ —— Arial 只有 ASCII，汉字画不出来 ⇒
   * 屏幕上是**一块空白** ✗；②黑块盖在 y=24~45 ✗ —— 正好把**版本号**（26~49）盖掉 ✗✓。
   * 现在：只盖**最下面一行**（状态行 ✓），并且用 uiText() 中英混排渲染 ✓（汉字画得出 ✓）。 */
  /* ★2026-09-25 二次修（主人报「我已刷 的刷字没有完整渲染、偏旁没了」✗）：
   * 16px 宋体点阵对「刷」这种复杂字会**掉右边的刂** ✗ ⇒ 横幅改用 **24px 点阵** ✓（完整 ✓）。
   * 位置也改成盖**标题那一行**（y=0~23 ✓）—— 标题是装饰性的，临时被反馈文字顶掉没关系 ✓，
   * 而**版本号**（y=25~48）**完全不受影响** ✓✓。 */
  int w = 0;
  const unsigned char* q = (const unsigned char*)msg;
  while (*q) { if (*q < 0x80) { w += uiAsciiW((char)*q, true); q++; } else { w += 24; q += 3; } }
  int x = (128 - w) / 2; if (x < 0) x = 0;
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.setColor(BLACK);
  oled.fillRect(0, 0, 128, 24);
  oled.setColor(WHITE);
  uiText24(x, 0, msg);
  oled.display();
  delay(1000);
}

#else  /* ============ USE_OLED = 0：没有 OLED 也能跑（这几件事都变成空动作 ✓）============ */
static void stDraw() { }
void bootSelfTest() {
  /* ★没有屏也不该丢掉这几件事：它们同时是黑匣子的数据（面包屑 / 按键 / 配置状态 ✓）*/
  stCfg = (mon.magic == MON_MAGIC);
  /* ★2026-09-29（审计提醒）：这里**照样扫一遍 I2C 总线** —— 
   *   否则 stI2c 恒为 false ⇒ /log 与 /rom 会报"总线=✗"，而 /i2c 其实是能扫到的 ✗（自检说谎）*/
  stI2c = false;
  for (uint8_t a = 0x03; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && a == 0x3C) { stI2c = true; break; }
  }
  bbPhase(PH_WIRE);
  pinMode(BTN_PIN, INPUT_PULLUP);
  stBtn = (digitalRead(BTN_PIN) == HIGH);
  stNet = (WiFi.status() == WL_CONNECTED);
  /* 原来这里 delay(700) 是"让人看清自检页" ✓ —— 没屏就不用等了，开机更快 ✓ */
}
void oledBanner(const char* msg) { (void)msg; }
#endif /* USE_OLED */

/* ===== NTP 对时（③）：连上 WiFi 后校时，东八区 ✓ ===== */
static void clockStart() {
  /* 2026-09-26 修：原来只给域名 ✗ —— 只要那一刻 DNS/网络有点抖就**永远不校时** ✗（主人就撞上了：屏上一直 7s ✗）。
   * 现在：①**阿里云 NTP 的 IP 放第一位**（绕开 DNS ✓）②域名兜底 ③loop 里每 30 秒重试一次 ✓✓ */
  configTime(8 * 3600, 0, "203.107.6.88", "ntp.aliyun.com", "cn.pool.ntp.org");
  Serial.println("[NTP] 已请求校时（阿里云 NTP / pool.ntp.org，东八区）");
}
static bool clockReady() { return time(nullptr) > 1600000000L; }
static String two2(int v) { return v < 10 ? String("0") + String(v) : String(v); }
static String clockHM(time_t t)   { struct tm x; localtime_r(&t, &x); return two2(x.tm_hour) + ":" + two2(x.tm_min); }
static String clockStamp(time_t t){ struct tm x; localtime_r(&t, &x);
  return String(1900 + x.tm_year) + "-" + two2(x.tm_mon + 1) + "-" + two2(x.tm_mday) + " " +
         two2(x.tm_hour) + ":" + two2(x.tm_min) + ":" + two2(x.tm_sec); }

static String agoText(unsigned long sec) {   // 把"多少秒前"变成 12s / 5m / 1h（小字用 ✓）
  if (sec < 60)               return String(sec) + "s";
  if (sec < 3600)             return String(sec / 60) + "m";
  return String(sec / 3600) + "h";
}

#if USE_OLED
void oledUpdate() {                       // 把"最新版本 / 有没有新包 / 上传时间"画到 OLED 上
  oled.clear();
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
#if OLED_SMALL
  /* ===== 128x32 版面（0.91" 那种小屏）：两行就满了 —— 标题 16px + 「版本 ｜ 状态」16px =====
   * ★2026-09-30 适配（主人「接着维护当前项目适配性系统」）：原版面按 128x64 写死 ✗
   *   ⇒ 32 行屏上 y=48 那行直接画到屏幕外面（等于什么都没有 ✗）⇒ 现在按高度分岔 ✓ */
  uiText(0, 0, ext.title);                 // 标题（可配，字模里有 ✓）
  const char* ver = mon.latest[0] ? mon.latest : "...";
  oled.setFont(ArialMT_Plain_16);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 16, String(ver));
  bool netOk = (WiFi.status() == WL_CONNECTED);
  const char* st = !netOk ? "离线" : (hasNew() ? "有新版本" : (mon.latest[0] ? "已是最新" : "检查中"));
  /* 状态字**只在真放得下时才画** —— 版本号长度会变（v4.0.25 / 3.1.2 / …）⇒ 必须先算再画 ✓
   * （小屏上宁可不显示状态，也不能两段字撞成一坨 ✗ —— 这就是 9-25 那次"乱码"的教训 ✓）*/
  if ((int)oled.getStringWidth(String(ver)) + 6 + uiStrW16(st) <= 128) {
    uiText(128 - uiStrW16(st), 16, st);
  }
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
#else
  /* 版面（128x64）：标题 24px(0-23) → 分隔线(24) → 版本 24px(26-49) → 状态 16px(48-63)
   * ★2026-09-25：删掉原来画在 y=54 的页脚 ✗ —— 它和状态行(y=45~60)**重叠**，
   *   屏幕上看起来就是一坨乱码 ✗（主人报「这底下这些都是些什么玩意」✗）。 */
  uiText24(0, 0, ext.title);               // ★标题可配（默认"系统包监测"；字模里有这几个字 ✓）
  oled.drawLine(0, 24, 127, 24);

  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.setFont(ArialMT_Plain_24);
  oled.drawString(0, 25, mon.latest[0] ? String(mon.latest) : String("..."));

  /* ★2026-09-25 屏上加料（不用主人动手 ✓）：状态行左边说"结果"、右边小字说"它还在干活"（✓）
   * ①Wi-Fi 掉了 → 直接写「离线」（一眼分清"没更新"和"网断了" ✓）
   * ②有新包 → 右边显示包的**上传日期**（如 9/23 ✓）
   * ③没新包 → 右边显示**上次检查过了多久**（12s/5m/1h ✓）—— 数字在变＝监测真的在跑 ✓✓ */
  bool netOk = (WiFi.status() == WL_CONNECTED);
  if (!netOk)                                        uiText(0, 48, "离线");
  else if (hasNew())                                 uiText(0, 48, "有新版本");
  else if (mon.latest[0])                            uiText(0, 48, "已是最新");
  else                                               uiText(0, 48, "检查中");

  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_RIGHT);
  String right;
  /* 2026-09-25 二次修（主人报「esp8266 的时间怎么不同步」）：
   * 原来显示的是**上次检查的时刻** —— 它只在每轮检查（10 分钟）时才更新 ⇒ 屏上时间长时间冻住 ⇒
   * 看着就像"时钟没同步"。现在改成**当前时间**，并让 loop 每分钟重画一次 ⇒ 真的一直在走。 */
  if (hasNew()) right = String(mon.latestTime[0] ? String(mon.latestTime).substring(5, 10) : "");
  else if (clockReady()) right = clockHM(time(nullptr));
  else {
    /* ★原来是 agoText(mon.lastOkAt) ✗ —— lastOkAt 是"开机第几秒"，不是"多久以前" ✗
     * ⇒ 屏幕上会冻在 "7s" 一动不走 ✗（主人问的正是这个 ✓）。改成真正的"距今多少秒" ✓ */
    unsigned long nowS = (millis() - bootMs) / 1000;
    right = agoText(nowS > mon.lastOkAt ? nowS - mon.lastOkAt : 0);
  }
  oled.drawString(127, 53, right);          // y=53 在版本号(y=25~48)下方 ⇒ 不重叠 ✓
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
#endif
  oled.display();
}
#else
void oledUpdate() { }                     // 没有 OLED：状态页这件事不存在 ✓（状态照样在网页/屏上 ✓）
#endif /* USE_OLED */

/* ===== 检测历史（①）：最近 6 次，存 EEPROM 512 起（老配置在 0~427 ✓ 不动 ✓）===== */
#define HIST_MAGIC 0x48495332UL             // ★"HIS2"：2026-09-29 每条形如加了 src（来源）⇒ 换 magic，老记录作废 ✓
#define HIST_OFF   512
#define HIST_N     6
/* ★2026-09-29：每条记录**带上"当时在盯哪个数据源"** —— 
 *   起因：本喵做"换数据源"验收时，板子指向 GitHub 抓到了 3.1.2 并如实记了一条"成功" ✓，
 *   但历史表**不显示来源** ⇒ 主人看到"成功 3.1.2"就以为查错了 ✗✓。
 *   ⇒ 加 src 字段（存 ext.preset 的快照）⇒ 表里多一列"来源"，再也不会看成出错 ✓
 *   ★结构变了 ⇒ magic 已经 +1（HIS2）⇒ 老记录自动作废 ✓（HistEnt 仍是 24 字节，Hist 大小没变 ✓）*/
struct HistEnt { uint32_t t; char ver[16]; uint8_t ok; uint8_t fails; uint8_t src; uint8_t rsv; };
struct Hist    { uint32_t magic; uint8_t n; uint8_t head; HistEnt e[HIST_N]; } hist;

void histLoad() {
  EEPROM.get(HIST_OFF, hist);
  if (hist.magic != HIST_MAGIC) {
    memset(&hist, 0, sizeof(hist));
    hist.magic = HIST_MAGIC;
    EEPROM.put(HIST_OFF, hist);
    EEPROM.commit();
    Serial.println("[HIST] 新建历史表 ✓");
  }
}
void histAdd(bool ok, const String &ver) {
  HistEnt &e = hist.e[hist.head];
  e.t     = clockReady() ? (uint32_t)time(nullptr) : (uint32_t)((millis() - bootMs) / 1000);
  strncpy(e.ver, ver.c_str(), sizeof(e.ver) - 1); e.ver[sizeof(e.ver) - 1] = 0;
  e.ok    = ok ? 1 : 0;
  e.fails = (uint8_t)(mon.failCount > 255 ? 255 : mon.failCount);
  e.src   = ext.preset;                       // ★记下"这条是盯哪个源得到的" ✓
  hist.head = (uint8_t)((hist.head + 1) % HIST_N);
  if (hist.n < HIST_N) hist.n++;
  EEPROM.put(HIST_OFF, hist);
  EEPROM.commit();
}

void doCheck(bool force) {
  if (checking) return;
  if (WiFi.status() != WL_CONNECTED) return;
  checking = true;
  String name, when, err;
  bool ok = fetchLatest(name, when, err);
  if (ok) {
    bool changed = (strcmp(name.c_str(), mon.latest) != 0);
    /* ★首次发现时刻：版本一变（或头一次抓到）就记下来 ⇒ 主人问"什么时候抓到的"永远答得上 ✓ */
    if (changed || ext.found[0] == 0) {
      String stamp = clockReady() ? clockStamp(time(nullptr))
                                  : (String("开机 ") + String((millis() - bootMs) / 1000) + " 秒");
      memset(ext.found, 0, sizeof(ext.found));
      stamp.toCharArray(ext.found, sizeof(ext.found) - 1);
      ext.foundUp = (millis() - bootMs) / 1000;
      extSave();
    }
    name.toCharArray(mon.latest, sizeof(mon.latest));
    when.toCharArray(mon.latestTime, sizeof(mon.latestTime));
    /* ★★2026-09-29：**没有水位线时就先建立水位线，别报警** ✓
     *   起因：本喵换了数据源做验收 ⇒ 抓到的 3.1.2 与旧水位线 v4.0.20 一比 ⇒ 立刻"有新包"+LED 常亮 ✗，
     *   但那个水位线是**上一个数据源**的，根本没有可比性 ✗✓。
     *   规矩：①水位线为空（新设备 / 刚换源）⇒ 第一次成功就把它设成当前值（＝认定"现在就是基线"）✓
     *        ②换数据源时 handleCfg 会把水位线清掉 ⇒ 自动落到这条规矩上 ✓ */
    if (mon.ack[0] == 0) {
      strncpy(mon.ack, mon.latest, sizeof(mon.ack) - 1);
      Serial.printf("[ROM] 首次成功 ⇒ 建立水位线 %s（不报警 ✓）\n", mon.ack);
    }
    mon.lastOkAt = (millis() - bootMs) / 1000;
    mon.failCount = 0;
    mon.lastErr[0] = 0;
    monSave();
    Serial.printf("[ROM] ok latest=%s (%s)%s\n", mon.latest, mon.latestTime, changed ? "  <== 变了" : "");
  } else {
    mon.failCount++;
    err.toCharArray(mon.lastErr, sizeof(mon.lastErr));
    monSave();
    Serial.printf("[ROM] fail #%u: %s\n", mon.failCount, mon.lastErr);
  }
  histAdd(ok, name);
  checking = false;
  oledUpdate();
  tjcPush(false);                          // ★串口屏：抓完把状态推过去（内部比对，没变就不发 ✓）
}

/* ==================================================================================
 * 串口屏（TJC/陶晶驰 TJC8048X270_011R，**只认 115200** 8N1）人机交互 —— 本次新增
 * ----------------------------------------------------------------------------------
 * 屏→ESP（每帧以 **3 个 0xFF** 结束，TJC 惯例 ✓）:
 *   #SCAN                               扫附近 WiFi，结果推给屏
 *   #WIFI <ssid>|<pass>                 连接并保存（| 分隔，密码可空）
 *   #DHCP                               改回自动获取
 *   #IP <ip>|<mask>|<gw>|<dns>          设静态 IP 并保存
 *   #CHECK  #ACK  #SKIP  #STATUS  #PING
 * ESP→屏：直接发 TJC 指令文本（tjcSend 自动补 3 个 0xFF；**发完就走、不等回包** ✓ 不阻塞主循环 ✓）
 * 组件名与屏工程手册约定一致：
 *   main : tTitle tNet mWifi mIp mRom mFix mCheck mAbout tFoot
 *   wifi : tW0…tW9 tWsel tSSID tPass tKeyNum tMsg bOk bBack
 *   ip   : tIp tMask tGw tDns bDhcp bStatic bSave bBack tMsg
 *   rom  : tVer tState tLog bAck bSkip bCheck bBack
 *   about: tAbout bBack
 * ★2026-09-26 修正：屏里**没有 kBd 这个控件** ✗（键盘由控件的 key 属性自动生成 ✓）⇒ 固件一个字都不引用它 ✓；
 *   tSSID / tKeyNum 是屏工程内部的隐藏工具控件 ⇒ ESP 侧不用碰（只写 tW0…tW9 / tWsel / tMsg ✓）。
 * ================================================================================== */
#if USE_TJC
String        tjcRxBuf = "";               // 收报缓冲（★限长 150 字节；绝不加大缓冲 ✗ 堆只有 ~20KB ✓）
uint8_t       tjcFfRun = 0;                // 连续 0xFF 计数（满 3 个 = 一帧结束 ✓）
unsigned long tjcRxAt = 0;                 // 最后一个字节的时刻（半帧超时丢弃 ✓）
unsigned long lastTjcPush = 0;             // 定期比对的节流（3 秒一次 ✓）
String        tjcLastSig = "\x01";         // 上次推过的状态指纹（★一样就不推 ✓）
#endif  /* ★2026-09-29：这几个收报缓冲移进开关里（审计提醒：=0 时留着白占 RAM 与堆碎片 ✓）*/

void doAck() {                             // "我已刷"：网页 /ack、板载键短按、屏 #ACK —— **共用这一份** ✓
  memset(mon.ack, 0, sizeof(mon.ack));
  strncpy(mon.ack, mon.latest, sizeof(mon.ack) - 1);
  if (mon.skip[0] && strcmp(mon.skip, mon.ack) == 0) mon.skip[0] = 0;   // 确认了就清掉同版本的"跳过" ✓
  monSave();
  ext.seen[0] = 0; extSave();              // ★"已刷"之后"我知道了"就没意义了 ⇒ 清掉 ✓
  oledUpdate();
  tjcPush(true);
}
void doSkip() {                            // "跳过这版"：网页 /skip、板载键长按、屏 #SKIP —— **共用这一份** ✓
  memset(mon.skip, 0, sizeof(mon.skip));
  strncpy(mon.skip, mon.latest, sizeof(mon.skip) - 1);
  monSave();
  ext.seen[0] = 0; extSave();              // ★同上 ✓
  oledUpdate();
  tjcPush(true);
}

/* ---- 状态推送：联网状态 / IP / 最新版本 / 有没有新包 / 连续失败（★只在变化时推 ✓）---- */
#if USE_TJC
void tjcPush(bool force) {
  if (!tjcReady) return;
  bool ok = (WiFi.status() == WL_CONNECTED);
  String netTxt = ok ? (String("已联网 ") + WiFi.localIP().toString()) : String("未联网");
  String verTxt = mon.latest[0] ? String(mon.latest) : String("（未获取）");
  String state;
  if (!ok)                 state = "离线";
  else if (hasNew())       state = "有新版本";
  else if (mon.latest[0])  state = "已是最新";
  else                     state = "检查中";
  String sig = netTxt + "|" + verTxt + "|" + state + "|" + String(mon.failCount);
  if (!force && sig == tjcLastSig) return;          // ★状态没变 ⇒ 一个字都不发 ✓
  tjcLastSig = sig;

  tjcSet("tTitle", ext.title);                      // ★标题可配 ✓
  tjcSet("tNet",   netTxt);                         // 磁贴页：联网状态 + IP ✓
  tjcSet("tVer",   verTxt);                         // ROM 页：最新版本 ✓
  tjcSet("tState", state);                          // ROM 页：有没有新包 ✓
  String foot = String("最新 ") + verTxt;
  if (mon.latestTime[0])   foot += String(" · ") + String(mon.latestTime);
  if (ok && mon.failCount) foot += String(" · 连续失败 ") + String(mon.failCount);
  tjcSet("tFoot", foot);                            // 磁贴页页脚 ✓
}

/* ---- #SCAN：扫附近 WiFi → **按信号强度排序** → 逐条推给屏（★扫完立刻 scanDelete ✓）---- */
void tjcDoScan() {
  tjcPage("wifi");
  tjcSet("tMsg", "正在扫描…");
  int n = WiFi.scanNetworks();               // 同步扫描（几秒）；内存占用最大的一步 ✓
  if (n < 0) n = 0;
  int m = (n > 24) ? 24 : n;                 // 最多排 24 个（★省内存：屏上只显示 10 行 ✓）
  static bool used[24];
  for (int i = 0; i < 24; i++) used[i] = false;
  for (int row = 0; row < 10; row++) {       // tW0…tW9 = 信号最强的 10 个 ✓
    int best = -1;
    for (int i = 0; i < m; i++) {
      if (used[i]) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    String txt = "";
    if (best >= 0) {
      used[best] = true;
      String ssid = WiFi.SSID(best);
      if (!ssid.length()) ssid = "(隐藏)";
      if (ssid.length() > 22) ssid = ssid.substring(0, 22);
      txt = ssid + "  " + String(WiFi.RSSI(best)) + "dBm";
      tjcSsid[row] = ssid;              // ★记下来：坐标点到第 N 行就能直接用它 ✓
    } else {
      tjcSsid[row] = "";
    }
    tjcSet(String("tW") + String(row), txt);
  }
  WiFi.scanDelete();                         // ★立刻释放扫描结果（不释放就是几 KB 一直挂着 ✗）
  tjcSet("tMsg", String("扫描到 ") + String(n) + " 个，点一行选 SSID");
  tjcSet("tWsel", "");
  Serial.printf("[TJC] #SCAN 扫到 %d 个 AP（已 scanDelete ✓）\n", n);
}

/* ---- #WIFI <ssid>|<pass>：存进 cfg（0 偏移、与 ota-base 同布局 ✓）并当场连（超时 15 秒 ✓）---- */
/* ★顺手削掉 SSID 尾巴上的信号值：
 *   协议规定屏上 tW0~tW9 的文本是 "<ssid>  -45dBm"（为了好看 ✗），屏工程点一行后**很可能整串**发回来 ✓，
 *   那样会拿着 "MyWiFi  -45dBm" 去连 ⇒ 必然连不上 ✗。这里把结尾的 "  -45dBm" 削掉 ✓
 *   （真实 SSID 不会以 "-45dBm" 结尾 ⇒ 削错的可能性可以忽略 ✓；屏工程自己削过也没关系，这里是幂等的 ✓） */
static void stripRssiTail(String &s) {
  if (!s.endsWith("dBm")) return;
  int j = s.length() - 3;                    // 指向 'B'
  int k = j;
  while (k > 0 && ((s[k - 1] >= '0' && s[k - 1] <= '9') || s[k - 1] == '-' || s[k - 1] == ' ')) k--;
  if (k > 0 && k < j) s = s.substring(0, k);
}

void tjcDoWifi(const String &arg) {
  int bar = arg.indexOf('|');
  String ssid = (bar >= 0) ? arg.substring(0, bar) : arg;
  String pass = (bar >= 0) ? arg.substring(bar + 1) : String("");
  ssid.trim(); pass.trim();
  stripRssiTail(ssid);                       // ★"MyWiFi  -45dBm" ⇒ "MyWiFi" ✓
  tjcPage("wifi");
  if (!ssid.length() || ssid.length() > 32) { tjcSet("tMsg", "SSID 无效（1~32 字符）"); return; }
  if (pass.length() > 64)                   { tjcSet("tMsg", "密码太长（最多 64 字符）"); return; }
  tjcSet("tMsg", "连接中…最多 15 秒");
  saveCfg(ssid, pass);                       // ★saveCfg 内部按 ssid[33]/pass[65] 截断 ⇒ 不会溢出 ✓
  Serial.printf("[TJC] #WIFI ssid=%s pass=%u 字节\n", cfg.ssid, (unsigned)strlen(cfg.pass));
  WiFi.disconnect();
  delay(30);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostName());                 // WiFi.begin() 会清掉 hostname，重设一次 ✓
  applyStaticIp();                           // 有静态 IP 就先配好（DHCP 时它直接返回 ✓）
  WiFi.begin(cfg.ssid, cfg.pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000UL) { ledOn(); delay(120); ledOff(); delay(130); }
  if (WiFi.status() == WL_CONNECTED) {
    clockStart();                            // ★连上之后才校时（没网就调会偶发崩 ✗）
    tjcSet("tMsg", String("已连接 ") + WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + "dBm");
    Serial.printf("[TJC] #WIFI 成功 ip=%s（不做整机重启 ✓）\n", WiFi.localIP().toString().c_str());
  } else {
    tjcSet("tMsg", "连接失败：检查密码或信号");
    Serial.println("[TJC] #WIFI 失败（配置已存，重启会再试）");
  }
  tjcPush(true);
}

/* ---- #IP <ip>|<mask>|<gw>|<dns>：存进 768 偏移的新结构体，重启后在联网前 config ✓ ---- */
void tjcDoIp(const String &arg) {
  tjcPage("ip");
  int p1 = arg.indexOf('|');
  int p2 = (p1 >= 0) ? arg.indexOf('|', p1 + 1) : -1;
  int p3 = (p2 >= 0) ? arg.indexOf('|', p2 + 1) : -1;
  if (p1 < 0 || p2 < 0 || p3 < 0) { tjcSet("tMsg", "格式：IP|掩码|网关|DNS"); return; }
  IPAddress ip, mask, gw, dns;
  if (!parseIp(arg.substring(0, p1), ip) || !parseIp(arg.substring(p1 + 1, p2), mask) ||
      !parseIp(arg.substring(p2 + 1, p3), gw) || !parseIp(arg.substring(p3 + 1), dns)) {
    tjcSet("tMsg", "IP 格式不对（四段 0~255）");
    return;
  }
  net.magic = NET_MAGIC;
  net.useStatic = 1;
  net.ip = (uint32_t)ip; net.mask = (uint32_t)mask; net.gw = (uint32_t)gw; net.dns = (uint32_t)dns;
  netSave();                                 // ★只写 768 起的尾巴，老布局一个字节不动 ✓
  tjcSet("tIp",   ip.toString());   tjcSet("tMask", mask.toString());
  tjcSet("tGw",   gw.toString());   tjcSet("tDns",  dns.toString());
  tjcSet("tMsg", "已保存静态IP，重启生效…");
  Serial.printf("[TJC] #IP 存 EEPROM %d 起：%s / %s  网关 %s  DNS %s\n",
                NET_OFF, ip.toString().c_str(), mask.toString().c_str(),
                gw.toString().c_str(), dns.toString().c_str());
  delay(800);
  ESP.restart();                             // ★静态 IP 只在"联网前"config 才干净 ⇒ 存完重启一次（先告诉屏了 ✓）
}

/* ---- #DHCP：改回自动获取（重启后不再调 WiFi.config ✓）---- */
void tjcDoDhcp() {
  tjcPage("ip");
  net.magic = NET_MAGIC;
  net.useStatic = 0;
  netSave();
  tjcSet("tMsg", "已切回 DHCP，重启生效…");
  Serial.println("[TJC] #DHCP 已存（重启后走 DHCP ✓）");
  delay(800);
  ESP.restart();
}

/* ---- 协议分发：命令头大写比对；参数原样（SSID/密码/点分 IP 都不区分大小写 ✓）---- */
void tjcHandle(const String &raw) {
  String line = raw; line.trim();
  if (!line.length() || line.charAt(0) != '#') return;   // 不是协议帧（比如屏的 connect 回包）⇒ 忽略 ✓
  String cmd = line, arg = "";
  int sp = line.indexOf(' ');
  if (sp > 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }
  cmd.toUpperCase();
  Serial.printf("[TJC] 收到 %s\n", cmd.c_str());
  if      (cmd == "#SCAN")   tjcDoScan();
  else if (cmd == "#WIFI")   tjcDoWifi(arg);
  else if (cmd == "#DHCP")   tjcDoDhcp();
  else if (cmd == "#IP")     tjcDoIp(arg);
  else if (cmd == "#CHECK")  { tjcSet("tState", "检查中…"); doCheck(true); tjcPush(true); }
  else if (cmd == "#ACK")    { tjcSet("tLog", String("已确认 ") + String(mon.latest)); doAck(); }
  else if (cmd == "#SKIP")   { tjcSet("tLog", String("已跳过 ") + String(mon.latest)); doSkip(); }
  else if (cmd == "#STATUS") { tjcPush(true); }
  else if (cmd == "#PING")   { tjcSend("#PONG"); }
  else                       Serial.println("[TJC] 未知命令（忽略 ✓）");
}

/* ★★ 触摸坐标表（2026-09-26 新增 —— "零 GUI 开发"的核心 ✓✓）
 * 坐标全部来自【工程文件实测】(verify_hmi.py 读出来的 x/y/w/h ✓)，不是猜的 ✓
 * 这样屏上不需要给任何控件绑事件 ✗ ⇒ 也就不需要点那个上位机 GUI 了 ✓✓
 * ★维护：改了控件位置 ⇒ 只需改这张表 ✓（改坐标不用碰屏工程 ✓）
 */
static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}
void tjcTouch(int x, int y, int ev) {
  (void)ev;
  const char *pg = tjcCurPage;            // 固件自己记的"当前页"✓（tjcPage() 会更新 ✓）
  Serial.printf("[TJC] 触摸 (%d,%d) @%s\n", x, y, pg);
  if (!strcmp(pg, "main")) {
    /* 6 个磁贴（坐标来自文件 ✓ 控件都是 100×30 ✓）*/
    if      (inRect(x, y,  14,  74, 100, 30)) { tjcDoScan(); }        // 磁贴1 = 扫 WiFi → 进 wifi 页 ✓
    else if (inRect(x, y, 272,  80, 100, 30)) tjcHandle("#STATUS");
    else if (inRect(x, y, 528,  80, 100, 30)) tjcHandle("#CHECK");
    else if (inRect(x, y,  16, 240, 100, 30)) tjcHandle("#ACK");
    else if (inRect(x, y, 272, 240, 100, 30)) tjcHandle("#SKIP");
    else if (inRect(x, y, 528, 240, 100, 30)) tjcHandle("#PING");
  } else if (!strcmp(pg, "wifi")) {
    /* 10 行 SSID：竖排 x=16，y=30+28i，100×30 ✓ */
    for (int i = 0; i < 10; i++) {
      if (inRect(x, y, 16, 30 + i * 28, 100, 30)) {
        if (tjcSsid[i].length()) {
          tjcSet("tWsel", tjcSsid[i]);
          tjcSet("tMsg", String("已选 ") + tjcSsid[i]);
          Serial.printf("[TJC] 选中 SSID: %s\n", tjcSsid[i].c_str());
        }
        break;
      }
    }
  } else if (!strcmp(pg, "ip")) {
    if      (inRect(x, y,  16,  40, 100, 50)) tjcHandle("#DHCP");      // b3 = 切回 DHCP ✓
    else if (inRect(x, y, 176,  40, 100, 50)) tjcHandle("#IP ");       // b2 = 保存静态 IP（四段见下 ✓）
    else if (inRect(x, y, 336,  40, 100, 50)) tjcPage("main");         // b1 = 返回 ✓
    else if (inRect(x, y,  16, 100, 100, 50)) tjcPage("main");         // b0 = 返回 ✓
  } else if (!strcmp(pg, "rom")) {
    if      (inRect(x, y, 450, 300, 100, 50)) tjcHandle("#CHECK");
    else if (inRect(x, y, 300, 380, 100, 50)) tjcHandle("#ACK");
    else if (inRect(x, y, 450, 380, 100, 50)) tjcHandle("#SKIP");
  } else if (!strcmp(pg, "about")) {
    if (inRect(x, y, 300, 380, 100, 50)) tjcPage("main");
  }
}

/* ---- loop 里每圈调一次：**非阻塞**收报（一次只吃"已经到"的字节，绝不等 ✗）---- */
/* ★★2026-09-26 新增：触摸坐标驱动（★彻底摆脱"给控件绑事件" ✓✓）
 * 原理：屏发 `sendxy=1` 之后，任何触摸都会把它自己的 (x,y) 直接吐到串口 ✓
 *       帧格式：0x67 xH xL yH yL event FF FF FF（event: 1=按下 0=抬起 ✓）
 *       ⇒ 本喵按【已知的控件坐标】把坐标翻译成动作 ✓✓
 * ★好处：不需要给任何控件绑事件 ✗ ⇒ 也就不需要点那个上位机 GUI 了 ✓✓
 * ★注意：屏【重启后 sendxy 会重置回 0】✗ ⇒ 所以开机要自动发一遍（见 setup ✓）
 */
void tjcTouch(int x, int y, int ev);      // 前置声明 ✓

void tjcPoll() {
  if (!tjcReady) return;
  while (tjc.available()) {
    int c = tjc.read();
    if (c < 0) break;
    tjcRxAt = millis();
    /* ★触摸坐标帧：0x67（TJC/Nextion 的"触摸事件上报"✓）—— 单独走二进制路径 ✓ */
    if (c == 0x67 && tjcFfRun == 0) {
      int b[4]; int got = 0;
      unsigned long t0 = millis();
      while (got < 4 && millis() - t0 < 60) {      // 只等 60ms，绝不阻塞 loop ✗
        if (tjc.available()) { int v = tjc.read(); if (v >= 0) b[got++] = v; }
      }
      if (got == 4) {
        int tx = (b[0] << 8) | b[1];
        int ty = (b[2] << 8) | b[3];
        int ev = tjc.available() ? tjc.read() : 1;  // event 字节（1=按下 ✓）
        while (tjc.available() && tjc.peek() == 0xFF) tjc.read();   // 吃掉帧尾 ✓
        if (ev == 1) tjcTouch(tx, ty, 1);          // 只在【按下】时响应 ✓（避免抬起重复触发 ✓）
      }
      tjcFfRun = 0;
      continue;
    }
    if (c == 0xFF) {                         // 帧尾：连续 3 个 0xFF 才算一帧结束 ✓
      if (++tjcFfRun >= 3) {
        tjcFfRun = 0;
        String line = tjcRxBuf; tjcRxBuf = "";
        if (line.length()) tjcHandle(line);
      }
      continue;                              // 0xFF 本身不进缓冲 ✓（GB2312 也不会有 0xFF ✓）
    }
    tjcFfRun = 0;
    if (tjcRxBuf.length() < 150) tjcRxBuf += (char)c;
    else                         tjcRxBuf = "";      // 报文超长：整条丢掉（★保护堆 ✓）
  }
  if (tjcRxBuf.length() && millis() - tjcRxAt > 800) tjcRxBuf = "";   // 半帧超时（掉字节了）⇒ 丢弃 ✓
}

#else  /* ===== USE_TJC = 0：状态推送/收报都变成空动作（网页与 OLED 一切照旧 ✓）===== */
void tjcPush(bool force) { (void)force; }
void tjcPoll() { }
#endif /* USE_TJC */

// ---------------------------------------------------------------- 页面
/* ==================================================================================
 *  网页 UI（2026-09-29 重做）
 * ----------------------------------------------------------------------------------
 * ★设计取舍（都是被板子的硬约束逼出来的，不是好看不好看的问题 ✓）：
 *  ① **整页从 PROGMEM 流式发**（chunked）—— 板子常态只剩 ~18KB 堆，
 *     在 RAM 里拼一个 10KB 的 String 会直接把堆打崩 ✗（历史上就是这么崩过 ✓）
 *     ⇒ 页面一个字节都不占堆（见 uiBegin / uiEnd ✓）
 *  ② **服务端不做任何动态渲染**：HTML/CSS/JS 全是静态字面量，
 *     会变的东西统一交给 JS 去轮询 `/rom`（5 秒一次 ✓）⇒ 固件更简单、页面反应更快 ✓
 *  ③ **零外部资源**（不引 CDN / 不外链字体 / 图标内联 SVG）——
 *     板子在内网、可能根本没外网 ⇒ 页面必须**离线也完整** ✓
 *  ④ 触控目标 ≥44px、`aria-live` 提示、`prefers-color-scheme` 跟随深浅色、
 *     `prefers-reduced-motion` 关动效 —— 手机上单手能点，电脑上不刺眼 ✓
 *  ⑤ 交互动作全部走 fetch 后台提交 ⇒ **点一下不刷整页**，只弹一条 toast ✓（秒级反馈 ✓）
 * ================================================================================== */

/* 网页资源（MINI_CSS / UI_HEAD / UI_BODY / UI_TAIL）全在 web_ui.h 里 —— 
 * ★不要搬回本文件 ✗：ctags 会把 JS 的 `function esc(s){` 当成 C++ 函数定义并插入假原型 ⇒ 编译失败 ✗
 *   （2026-09-29 实测的报错：error: 'function' does not name a type ✓）*/
#include "web_ui.h"

/* 拼出"这个版本在网页上的地址"（手机点一下就能打开）—— 
 * ★通用化后交给 extWebUrl()：配了 webBase 就用它（GitHub 用 https://github.com/O/R/releases ✓），
 *   没配才退回 host+path（OpenList 那套 ✓） */
String romPageUrl(const char* ver) { return extWebUrl(ver); }

/* （原来的 stateBlock() 已删：状态文本现在由页面的 JS 从 /rom 现场拼 —— 服务端不用再拼 String ✓） */

/* ---------- 主页：整页从 PROGMEM 流式发（chunked），一个字节都不占堆 ✓ ---------- */
static bool uiBegin(PGM_P ctype) {
  if (server.chunkedResponseModeStart(200, ctype)) return true;   // HTTP/1.1 ⇒ 走 chunked ✓
  /* HTTP/1.0 的老客户端不支持 chunked ⇒ 只能给个短提示页
   * （★绝不为了它把 12KB 整页塞进 RAM ✗ —— 那正是历史上把堆打崩的写法 ✓） */
  server.send(200, "text/html; charset=utf-8",
              F("<!doctype html><meta charset='utf-8'><div style='font:16px system-ui;padding:24px'>"
                "请用支持 HTTP/1.1 的浏览器打开本页（Edge / Chrome / 手机浏览器 ✓）</div>"));
  return false;
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");   // 刷完固件立刻能看到新界面（12KB 在内网就是一瞬 ✓）
  if (!uiBegin("text/html; charset=utf-8")) return;
  server.sendContent_P(UI_HEAD);
  server.sendContent_P(UI_BODY);
  server.sendContent_P(UI_TAIL);
  server.chunkedResponseFinalize();                 // 发 0 长度块收尾 ✓
}

void handleAck() {                           // 网页"我已刷" ⇒ 与屏 #ACK / 板载键短按**同一份实现** ✓
  doAck();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "ok");
}
void handleSkip() {                          // 网页"跳过这版" ⇒ 与屏 #SKIP / 板载键长按**同一份实现** ✓
  doSkip();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "ok");
}

void handleCfg() {
  /* ★2026-09-29：**换数据源时把水位线作废** ✓ ——
   *   水位线 v4.0.20 是"OpenList 的 v4.0.20"，换成 GitHub 之后跟它比毫无意义 ✗
   *   （不改的话：换过去第一轮就报"有新包" + LED 常亮 ⇒ 假警报 ✗）。
   *   作废后由 doCheck 的"首次成功即建立水位线"接管 ✓（除非这次显式传了 ack= ✓）*/
  char oldHost[48], oldPath[96];
  strncpy(oldHost, mon.host, sizeof(oldHost) - 1); oldHost[sizeof(oldHost) - 1] = 0;
  strncpy(oldPath, mon.path, sizeof(oldPath) - 1); oldPath[sizeof(oldPath) - 1] = 0;
  uint8_t oldPreset = ext.preset;
  /* ★2026-09-29：先应用"预设"（它会把 jsonKey/timeKey/method 重置成该预设的默认值 ✓），
   *   再让显式传上来的字段覆盖它 ⇒ 网页上"点预设"与"手改细节"两种用法都能work ✓ */
  if (server.hasArg("preset")) {
    int p = server.arg("preset").toInt();
    if (p >= 0 && p <= PRESET_CUSTOM) { extApplyPreset((uint8_t)p); extSave(); }
  }
  if (server.hasArg("ack")) {                       // 手工设定水位线（也方便自测"有新包"）
    String a = server.arg("ack"); a.trim();
    memset(mon.ack, 0, sizeof(mon.ack));
    a.toCharArray(mon.ack, sizeof(mon.ack) - 1);
  }
  if (server.hasArg("path")) {
    String p = server.arg("path"); p.trim();
    if (p.length() && p.startsWith("/")) { memset(mon.path, 0, sizeof(mon.path)); p.toCharArray(mon.path, sizeof(mon.path) - 1); }
  }
  if (server.hasArg("host") && server.arg("host").length()) {
    String h = server.arg("host"); h.trim();
    memset(mon.host, 0, sizeof(mon.host)); h.toCharArray(mon.host, sizeof(mon.host) - 1);
  }
  if (server.hasArg("interval")) {
    uint32_t iv = server.arg("interval").toInt();
    if (iv >= 60) mon.interval = iv;
  }
  /* ---- 通用化新增（2026-09-29）---- */
  if (server.hasArg("title")) {
    String t = server.arg("title"); t.trim();
    if (t.length()) { memset(ext.title, 0, sizeof(ext.title)); t.toCharArray(ext.title, sizeof(ext.title) - 1); }
  }
  if (server.hasArg("jsonKey")) {
    String k = server.arg("jsonKey"); k.trim();
    if (k.length()) { memset(ext.jsonKey, 0, sizeof(ext.jsonKey)); k.toCharArray(ext.jsonKey, sizeof(ext.jsonKey) - 1); }
  }
  if (server.hasArg("timeKey")) {
    String k = server.arg("timeKey"); k.trim();
    if (k.length()) { memset(ext.timeKey, 0, sizeof(ext.timeKey)); k.toCharArray(ext.timeKey, sizeof(ext.timeKey) - 1); }
  }
  if (server.hasArg("method")) ext.method = (server.arg("method").toInt() == M_POST) ? M_POST : M_GET;
  if (server.hasArg("webBase")) {                   // 留空 = 按 host+path 自动拼 ✓
    String w = server.arg("webBase"); w.trim();
    memset(ext.webBase, 0, sizeof(ext.webBase));
    if (w.length()) w.toCharArray(ext.webBase, sizeof(ext.webBase) - 1);
  }
  if (server.hasArg("body")) {                      // 留空 = POST 时按 preset 自动生成 ✓
    String b = server.arg("body"); b.trim();
    memset(ext.body, 0, sizeof(ext.body));
    if (b.length()) b.toCharArray(ext.body, sizeof(ext.body) - 1);
  }
  monSave();
  extSave();
  /* ★换源就作废水位线（显式传了 ack= 的除外 ✓）*/
  bool srcChanged = (strcmp(oldHost, mon.host) != 0) || (strcmp(oldPath, mon.path) != 0) ||
                    (oldPreset != ext.preset);
  if (srcChanged && !server.hasArg("ack")) {
    mon.ack[0] = 0;
    mon.skip[0] = 0;
    monSave();
    Serial.println("[CFG] 数据源变了 ⇒ 水位线作废（下一轮成功即建立新基线，不会假报警 ✓）");
  }
  Serial.printf("[CFG] 源=%s host=%s path=%s 键=%s/%s 标题=%s\n",
                extPresetName().c_str(), mon.host, mon.path, ext.jsonKey, ext.timeKey, ext.title);
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "ok");
}
void handleReconfig() {                      // 清除 WiFi 配置 → 重启进配网 AP 模式
  clearCfg();
  server.send(200, "text/html; charset=utf-8",
              pageHeader("重新配网") + F("<h1>已清除配网信息</h1>"
                                        "<p class='h'>设备即将重启并开启配网热点 <span style='font-family:monospace'>ROMWatch-&lt;芯片号&gt;</span>（开放、无密码），"
                                        "连上后打开 <span style='font-family:monospace'>http://192.168.4.1/</span> 重新填写 WiFi ✓</p></div></body></html>"));
  delay(700);
  ESP.restart();
}

/* ★2026-09-29 改：**立刻回包、把抓包排到主循环里做** ✓
 *   原来这里 send 完就**当场阻塞**跑 doCheck(true)（TLS 握手最长 25 秒 ✗）⇒
 *   网页上按下【立即检查】要等十几秒才"转完"，期间整个服务器是哑的 ✗。
 *   现在只置一个标志，loop 里下一圈就做 ⇒ 按钮**秒回**，页面靠 5 秒轮询自己看到新结果 ✓ */
void handleCheck() {
  checkSoon = true;
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"checking\"}");
}

/* ★2026-09-29 新增：`/led?on=1|0|2` —— 手动点灯（强制 15 秒，期间主循环不抢 ✓）
 *   为什么要有它：主人报「有新版本怎么不会亮灯」✗ ⇒
 *   第一步就该**把"灯和 IO 是不是好的"和"逻辑要不要亮"分开** ✓ ——
 *   这个端点直接驱动引脚 ⇒ 灯亮＝硬件/IO 没问题、问题在判断；灯不亮＝就是灯/引脚的事 ✓✓ */
void handleLed() {
  String v = server.arg("on");
  int want = v.length() ? v.toInt() : 2;              // 缺省 = 翻转
  bool target;
  if (want == 2) {
    bool cur = (digitalRead(LED_PIN) == LOW);         // 当前是亮吗
    target = !cur;
  } else {
    target = (want != 0);
  }
  ledForceUntil = millis() + 15000UL;                 // 15 秒内主循环别抢灯 ✓
  if (target) ledOn(); else ledOff();
  delay(30);
  String j = String("{\"ok\":true,\"want\":") + (target ? "true" : "false") +
             ",\"pin\":" + String(LED_PIN) +
             ",\"level\":" + String(digitalRead(LED_PIN)) +
             ",\"lit\":" + String(digitalRead(LED_PIN) == LOW ? "true" : "false") +
             ",\"forcedMs\":15000,\"hasNew\":" + String(hasNew() ? "true" : "false") + "}";
  server.send(200, "application/json", j);
  Serial.printf("[LED] 手动点灯 pin=%d lit=%d（强制 15 秒）\n", LED_PIN, digitalRead(LED_PIN) == LOW);
}

/* ★2026-09-29 新增：`/seen` —— "我知道了（关灯）"
 *   语义＝**我看到这个提醒了，把灯摁灭** ✓；**不动**水位线（那还是"没刷"✗）、**不动**跳过 ✓、
 *   页面上"有新版本"照样在 ✓ ⇒ 真出了更新的版本，灯又会亮 ✓✓ */
void handleSeen() {
  if (server.hasArg("clear")) {             // ★撤销"我知道了"⇒ 灯恢复报警（自测/反悔都用得上 ✓）
    ext.seen[0] = 0;
    extSave();
    server.send(200, "application/json",
                String("{\"ok\":true,\"cleared\":true,\"ledAlarm\":") +
                (ledAlarm() ? "true" : "false") + "}");
    Serial.println("[SEEN] 已撤销：灯恢复报警 ✓");
    return;
  }
  memset(ext.seen, 0, sizeof(ext.seen));
  strncpy(ext.seen, mon.latest, sizeof(ext.seen) - 1);
  extSave();
  ledForceUntil = 0;
  ledOff();                                 // 立刻灭（不等下一圈 ✓）
  oledUpdate();
  Serial.printf("[SEEN] 我知道了：%s（灯灭，页面仍显示有新版本 ✓）\n", ext.seen);
  server.send(200, "application/json",
              String("{\"ok\":true,\"seen\":\"") + ext.seen + "\",\"ledAlarm\":false}");
}

/* ===== 页面唯一的数据源（新页面每 5 秒拉一次这里 ✓）=====
 * ★原则：**能在服务端算好的就别让 JS 猜** ——
 *   时间戳直接给成"人看的字符串"（带时区）、阶段给成中文名、
 *   下次检查倒计时算好再给 ⇒ JS 只管画，不用做时区/时钟运算 ✓
 * ★兼容：老字段一个都没删 ✗（PC 侧的脚本还在读 ✓），只往后追加 ✓ */
void handleRom() {
  String j; j.reserve(2100);
  j += "{\"host\":\"" + jsonEsc(String(mon.host)) + "\",\"path\":\"" + jsonEsc(String(mon.path)) + "\",";
  j += "\"latest\":\"" + String(mon.latest) + "\",\"latestTime\":\"" + String(mon.latestTime) + "\",";
  j += "\"ack\":\"" + String(mon.ack) + "\",\"skip\":\"" + String(mon.skip) + "\",";
  j += "\"hasNew\":" + String(hasNew() ? "true" : "false") + ",";
  j += "\"interval\":" + String(mon.interval) + ",\"lastOkAt\":" + String(mon.lastOkAt) + ",";
  j += "\"fails\":" + String(mon.failCount) + ",\"lastErr\":\"" + jsonEsc(String(mon.lastErr)) + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",\"build\":\"" + String(FW_BUILD) + "\",";
  j += "\"up\":" + String((millis() - bootMs) / 1000) + ",";
  j += "\"dl\":\"" + dlReq + "\",";
  j += "\"boots\":" + String(bb.boots) + ",\"lastReset\":\"" + String(bb.resetReason) + "\",";
  j += "\"phase\":" + String(bb.phase) + ",\"wifiMs\":" + String(bb.wifiMs) + ",";
  j += "\"clock\":" + String(clockReady() ? "true" : "false") + ",";
  j += "\"now\":\"" + String(clockReady() ? clockStamp(time(nullptr)) : "") + "\",";
  /* ---- 以下为 0.3.0 新版页面新增（老字段保持原样 ✓）---- */
  uint32_t w = waitSecs();
  uint32_t el = (millis() - lastPoll) / 1000;
  j += "\"name\":\"" + hostName() + "\",\"ver\":\"" + String(FW_VERSION) + "\",";
  j += "\"chip\":\"" + chipHex() + "\",\"mac\":\"" + WiFi.macAddress() + "\",\"ssid\":\"" + jsonEsc(WiFi.SSID()) + "\",";
  j += "\"waitS\":" + String(w) + ",\"nextIn\":" + String(el < w ? w - el : 0) + ",";
  j += "\"checking\":" + String(checking ? "true" : "false") + ",\"checkSoon\":" + String(checkSoon ? "true" : "false") + ",";
  /* ---- 报警灯与"首次发现时刻"（2026-09-29 加：灯/时间这两类问题都要能远程量 ✓）---- */
  j += "\"led\":" + String(digitalRead(LED_PIN) == LOW ? "true" : "false") +
       ",\"ledPin\":" + String(LED_PIN) + ",\"ledForce\":" + String(millis() < ledForceUntil ? "true" : "false") +
       ",\"otaRunning\":" + String(otaRunning ? "true" : "false") +
       ",\"ledAlarm\":" + String(ledAlarm() ? "true" : "false") +
       ",\"seen\":\"" + jsonEsc(String(ext.seen)) + "\",";
  j += "\"found\":\"" + jsonEsc(String(ext.found)) + "\",\"foundUp\":" + String(ext.foundUp) + ",";
  /* ★2026-09-30（修「时间差了一分钟」时加的**远程仪表**）：屏上"当前画的是第几分钟"＋"上次重画距今几秒" ——
   *   判据：校好时表时 **scrMin 必须恒等于 now/60** ✓（修之前它会在每分钟里有几十秒 ≠ 当前分钟 ✗）；
   *   scrUp 正常应当 < 60 秒（它是"这一分钟那一刻画的"✓）。*/
  j += "\"scrMin\":" + String((long)lastShownMin) + ",\"scrUp\":" +
       String((millis() - lastScreenAt) / 1000) + ",";
  /* ---- 数据源预设 / 标题（2026-09-29 通用化时加，给新版网页的表单用 ✓）---- */
  j += "\"title\":\"" + jsonEsc(String(ext.title)) + "\",";
  j += "\"preset\":" + String(ext.preset) + ",\"presetName\":\"" + extPresetName() + "\",";
  j += "\"method\":" + String(ext.method) + ",\"jsonKey\":\"" + jsonEsc(String(ext.jsonKey)) + "\",";
  j += "\"timeKey\":\"" + jsonEsc(String(ext.timeKey)) + "\",\"webBase\":\"" + jsonEsc(String(ext.webBase)) + "\",";
  j += "\"tjcEn\":" + String(USE_TJC ? "true" : "false") + ",";   // ★没编屏就别让网页说"无应答（探测 115200）" ✗
  j += "\"tjcOk\":" + String((USE_TJC && tjcInfo.length()) ? "true" : "false") + ",\"tjcBaud\":" + String(tjcBaud) +
       ",\"tjcInfo\":\"" + jsonEsc(hexOf(tjcInfo)) + "\",";
  j += "\"lastBody\":\"" + jsonEsc(lastBody) + "\",";
  j += "\"bb\":{\"boots\":" + String(bb.boots) + ",\"reset\":\"" + jsonEsc(String(bb.resetReason)) + "\",";
  j += "\"phase\":" + String(bb.phase) + ",\"phaseTxt\":\"" + String(phName(bb.phase)) + "\",";
  j += "\"phaseLast\":" + String(bb.phaseLast) + ",\"phaseLastTxt\":\"" + String(phName(bb.phaseLast)) + "\",";
  j += "\"wifiMs\":" + String(bb.wifiMs) + ",\"oled\":" + String(bb.oled ? "true" : "false") +
       ",\"i2c\":" + String(bb.i2c ? "true" : "false") + ",\"btn\":" + String(bb.btn ? "true" : "false") +
       ",\"cfg\":" + String(bb.cfg ? "true" : "false") + ",\"net\":" + String(bb.net ? "true" : "false") + "},";
  j += "\"cur\":{\"oled\":" + String(stOled ? "true" : "false") + ",\"i2c\":" + String(stI2c ? "true" : "false") +
       ",\"btn\":" + String(stBtn ? "true" : "false") + ",\"cfg\":" + String(stCfg ? "true" : "false") +
       ",\"net\":" + String((stNet || WiFi.status() == WL_CONNECTED) ? "true" : "false") +
       ",\"phase\":" + String(bb.phase) + ",\"phaseTxt\":\"" + String(phName(bb.phase)) + "\"},";
  j += "\"hist\":[";
  for (int i = 0; i < hist.n; i++) {                       // ★新的在前（与服务端原来的显示顺序一致 ✓）
    int idx = (hist.head - 1 - i + HIST_N * 2) % HIST_N;
    HistEnt &e = hist.e[idx];
    if (i) j += ",";
    j += "{\"txt\":\"";
    j += clockReady() ? clockStamp((time_t)e.t) : (String(e.t) + " 秒（开机计时）");
    j += "\",\"ver\":\"" + jsonEsc(String(e.ver)) + "\",\"ok\":" + String(e.ok ? "true" : "false") +
         ",\"fails\":" + String(e.fails) + ",\"src\":" + String(e.src) + ",\"srcName\":\"" +
         (e.src == PRESET_GITHUB ? "GitHub" : e.src == PRESET_CUSTOM ? "自定义" : "OpenList") + "\"}";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

/* ★2026-09-26 新增：波特率自动扫 —— 主人把 TJC 屏接回来了，但屏不吭声 ✗。
 * 最常见原因就是**波特率不对**（代码里写的是 115200 ✗，而 TJC 出厂常见 9600 ✗）。
 * 这个端点逐个波特率喊 connect，屏在正确的那档会回 "comok …" ✓ ⇒ 不用拆线就能判定 ✓✓ */
/* ★2026-09-26 新增：线路体检 /tjcwire
 * 为什么需要它：**软件串口的"自收自发"回环测不出来** ✗ ——
 * Arduino SoftwareSerial 在**发送期间会关掉自己的接收中断** ✗✓（半双工）⇒ 自己发自己听必然是 0 字节 ✓。
 * 所以这里绕开串口库：**D7 直接打方波、用中断数 D5 上的边沿** ✓✓ ——
 *   · 把 D5–D7 短接 ⇒ 边沿数会接近 400 ✓ ⇒ 证明**两个脚 + 杜邦线**都好 ✓
 *   · 还是 0 ✗ ⇒ 问题在板子这两个脚本身（或线没通 ✓）
 * 这样就能把"屏的问题"和"板子/线的问题"**彻底切开** ✓✓
 * ★2026-09-26 修正：这里原来打的是 **D6(GPIO12)** ✗ —— D6 是**开机判 flash 电压**的脚，
 *   绝对不能用（被驱动成输出，开机可能直接起不来 ✗）⇒ 改成 **D7(GPIO13)** ✓
 *   （D7 正是屏 RX 那一根，接好线的板子上做这个体检就是"对着屏的接收脚打方波" ✓）。 */
#if USE_TJC
volatile unsigned long wireEdges = 0;
static void wireEdgeISR() { wireEdges++; }

void handleTjcWire() {
  tjc.end();                              // 让出软串口两个脚，纯 GPIO 测试 ✓
  delay(50);
  wireEdges = 0;
  /* ★引脚全部走适配层（2026-09-30）：原来这里写死 D5/D7 ✗ ⇒ 别人改过接线后，
   *   这个"体检"端点测的还是老脚 ⇒ 结论是假的 ✗✓ */
  pinMode(PIN_TJC_RX, INPUT);             // 屏的 TX 接到这个脚（板上当 RX 用 ✓）
  pinMode(PIN_TJC_TX, OUTPUT);            // 板往屏发 → 这个脚（★别用 D6 ✗）
  attachInterrupt(digitalPinToInterrupt(PIN_TJC_RX), wireEdgeISR, CHANGE);
  for (int i = 0; i < 200; i++) {         // 200 个方波 ≈ 400 个边沿 ✓
    digitalWrite(PIN_TJC_TX, HIGH); delayMicroseconds(250);
    digitalWrite(PIN_TJC_TX, LOW);  delayMicroseconds(250);
  }
  detachInterrupt(digitalPinToInterrupt(PIN_TJC_RX));
  unsigned long got = wireEdges;
  tjc.begin(115200);                      // 还回去（★屏只认 115200 ✓）
  Serial.printf("[WIRE] %s 打 200 个方波，%s 数到 %lu 个边沿\n",
                RW_STR(PIN_TJC_TX), RW_STR(PIN_TJC_RX), got);
  String j = "{\"ok\":true,\"edges\":" + String(got) + ",\"expect\":400,";
  if (got > 50)      j += "\"verdict\":\"D5/D7 两个脚与杜邦线都通 ✓ （若这样屏还不回话 ⇒ 问题在屏/屏的供电/接反）\"}";
  else if (got > 0)  j += "\"verdict\":\"只有零星边沿 ⇒ 接触不良或线太长 ✓\"}";
  else               j += "\"verdict\":\"一个边沿都没有 ⇒ ①D5-D7 没短接 ②或这两个脚/线有问题 ✓\"}";
  server.send(200, "application/json", j);
}

void handleTjcSweep() {
  const long bauds[] = { 9600, 115200, 19200, 38400, 57600, 4800, 2400, 230400 };
  String j = "{\"ok\":true,\"try\":[";
  bool first = true;
  int hit = 0;
  for (unsigned i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
    tjc.end();
    delay(60);
    tjc.begin(bauds[i]);
    delay(150);
    String r = tjcCmd("connect", 800);        // 屏正常会回 comok 开头的一串 ✓
    if (!first) j += ",";
    first = false;
    j += "{\"baud\":" + String(bauds[i]) + ",\"len\":" + String(r.length()) +
         ",\"resp\":\"" + hexOf(r) + "\"" +
         (r.indexOf("comok") >= 0 ? ",\"OK\":true" : "") + "}";
    if (r.indexOf("comok") >= 0 && hit == 0) hit = bauds[i];
  }
  /* ★2026-09-26 修正：扫完**必须把串口恢复成 115200** ✗→✓ ——
   * 原来最后一档停在 230400 就不管了 ⇒ 之后 tjcSend / tjcPush 全用**错的波特率**往屏上写 ✗
   * ⇒ 屏那头看到的是乱码，表现为"屏突然再也不动了" ✗（ESP 自己完全无感 ✗）。
   * 屏只认 115200 ✓ ⇒ 这里显式 end + begin(115200) 复位，并回给网页 restored:115200 ✓ */
  tjc.end();
  delay(60);
  tjc.begin(115200);
  delay(150);
  j += "],\"hit\":" + String(hit) + ",\"restored\":115200}";
  if (hit) { Serial.printf("[TJC] 屏在 %d 波特率上回话 ✓（已恢复 115200 ✓）\n", hit); }
  else     { Serial.println("[TJC] 没有一档回话（已恢复 115200 ✓）"); }
  server.send(200, "application/json", j);
}

#else  /* USE_TJC = 0：这两个体检端点如实说明"本固件没编串口屏" ✓（比 404 强多了 ✓）*/
void handleTjcWire()  { server.send(200, "application/json", NO_TJC); }
void handleTjcSweep() { server.send(200, "application/json", NO_TJC); }
#endif /* USE_TJC */

void handleLog() {                            // /log —— 纯文本，手机/电脑一眼看 ✓
  String t = "";
  t += "启动次数      : " + String(bb.boots) + "\n";
  t += "上次复位原因  : " + String(bb.resetReason) + "\n";
  /* ★2026-09-29 修：OTA 升级会把 RTC 用户内存清掉 ⇒ 升完级 phaseLast 恒为 0 ✗，
   *   原来这里会写成"0 = （没到达任何阶段）★ 上次就崩在这一步" ⇒ **明明是刚升级，看着像崩了** ✗。
   *   现在把 0 单独当"刚升级/首次上电"讲清楚 ✓ */
  t += "上次跑到阶段  : " + String(bb.phaseLast) + " = " + String(phName(bb.phaseLast));
  if (bb.phaseLast == 0)            t += "  （OTA 升级/首次上电会清掉 RTC 记忆 ⇒ 这一轮本来就没有崩溃记录 ✓）";
  else if (bb.phaseLast == PH_LOOP) t += "  （上次是正常跑到主循环后才重启的 ⇒ 不是开机崩溃 ✓）";
  else                              t += "  ★ 上次就崩在这一步 ✓✓";
  t += "\n";
  t += "本次已到阶段  : " + String(bb.phase) + " = " + String(phName(bb.phase)) + "\n";
  t += "上次自检      : 显示屏=" + String(bb.oled ? "OK" : "✗") + "  总线=" + String(bb.i2c ? "OK" : "✗") +
       "  按键=" + String(bb.btn ? "OK" : "✗") + "  配置=" + String(bb.cfg ? "OK" : "✗") + "\n";
  t += "上次连WiFi    : " + (bb.wifiMs ? String(bb.wifiMs) + " ms" : String("没连上 ✗")) + "\n";
  t += "本次已运行    : " + String((millis() - bootMs) / 1000) + " 秒\n";
  t += "本次自检      : 显示屏=" + String(stOled ? "OK" : "✗") + "  总线=" + String(stI2c ? "OK" : "✗") +
       "  按键=" + String(stBtn ? "OK" : "✗") + "  配置=" + String(stCfg ? "OK" : "✗") +
       "  网络=" + String(stNet ? "OK" : "✗") + "\n";
  t += "最新包        : " + String(mon.latest[0] ? mon.latest : "(还没抓到)") + "\n";
  /* ★2026-09-29 通用化：把"在盯什么"讲清楚（换数据源后一眼看得出 ✓）*/
  t += "数据源        : " + extPresetName() + "  " + String(mon.host) + String(mon.path) + "\n";
  t += "取值/时间键   : \"" + String(ext.jsonKey) + "\" / \"" + String(ext.timeKey) + "\"  (" +
       String(ext.method == M_POST ? "POST" : "GET") + ")\n";
  t += "页面标题      : " + String(ext.title) + "\n";
  t += "编译开关      : USE_TJC=" + String(USE_TJC) + "  USE_OLED=" + String(USE_OLED) + "\n";
  t += "连续失败      : " + String(mon.failCount) + (mon.lastErr[0] ? String("  原因: ") + mon.lastErr : String()) + "\n";
  t += "TLS缓冲       : 收 4096 / 发 512 字节（默认 16KB 会把堆吃干 ✗ 见 2026-09-26 修复）\n";
  t += "堆内存        : " + String(ESP.getFreeHeap()) + " B   信号: " + String(WiFi.RSSI()) + " dBm\n";
  t += "固件          : " + String(FW_NAME) + " " + String(FW_VERSION) + " build " + String(FW_BUILD) + "\n";
  server.send(200, "text/plain; charset=utf-8", t);
}

void handleDl() {                            // 请求下载：把"当前最新版本"标成待下载 ✓
  dlReq = String(mon.latest);
  Serial.println("[DL] 收到下载请求: " + dlReq);
  oledBanner("已请求下载");
  oledUpdate();
  server.send(200, "application/json", "{\"ok\":true,\"dl\":\"" + dlReq + "\"}");
}

void handleDlClear() {                       // 电脑端下载已发起 ⇒ 清标志 ✓
  dlReq = "";
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleI2cScan() {                    // /i2c —— 扫 I2C 总线，看 OLED 在不在（0x3C 常见）
  String j = "{\"ok\":true,\"sda\":\"" RW_STR(PIN_OLED_SDA) "\",\"scl\":\"" RW_STR(PIN_OLED_SCL) "\",\"found\":[";
  int n = 0;
  for (uint8_t a = 0x03; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (n++ > 0) j += ",";
      j += "\"0x" + String(a, HEX) + "\"";
    }
  }
  j += "],\"count\":" + String(n) + "}";
  server.send(200, "application/json", j);
}

#if USE_TJC
void handleTjc() {                        // /tjc?cmd=connect   （隔着 WiFi 遥控屏，看回包）
  String cmd = server.arg("cmd");
  if (!cmd.length()) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"need ?cmd=\"}"); return; }
  String resp = tjcCmd(cmd, 700);
  String echo = cmd; echo.replace("\"", "'");
  String j = "{\"ok\":true,\"cmd\":\"" + echo + "\",\"len\":" + String(resp.length()) +
             ",\"resp\":\"" + hexOf(resp) + "\"}";
  server.send(200, "application/json", j);
}

void handleTjcCmd() {                     // ★新增 /tjccmd?c=tNet.txt="hi"  —— 浏览器直接给屏发一条 TJC 指令（调试用 ✓）
  String cmd = server.arg("c");           // ★URL 解码：ESP8266WebServer 的 urlDecode 已把 %XX **和 '+' 当空格**都解好了 ✓
                                          //   （见 cores 的 Parsing-impl.h：encodedChar=='%' 转义，'+' → ' ' ✓）
  if (!cmd.length()) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"need ?c=\"}"); return; }
  cmd.replace("\r", ""); cmd.replace("\n", "");            // 不许塞换行（会破坏 TJC 指令 ✓）
  if (cmd.length() > 96) cmd = cmd.substring(0, 96);       // ★限长 96 字节（省堆、也防误发大包 ✓）
  String resp = tjcCmd(gbText(cmd), 300);                  // 中文自动转 GB2312 ✓；等 300ms 只为把回包带回去 ✓
  String echo = cmd; echo.replace("\\", "/"); echo.replace("\"", "'");
  String j = String("{\"ok\":true,\"cmd\":\"") + echo + "\",\"len\":" + String(resp.length()) +
             ",\"resp\":\"" + hexOf(resp) + "\"}";
  server.send(200, "application/json", j);
}

#else  /* USE_TJC = 0：遥控端点如实回"没编串口屏"，不给 404 让人瞎猜 ✓ */
void handleTjc()    { server.send(200, "application/json", NO_TJC); }
void handleTjcCmd() { server.send(200, "application/json", NO_TJC); }
#endif /* USE_TJC */

void handleStatus() {
  String j = "{";
  j += "\"name\":\"" + hostName() + "\",\"fw\":\"" + String(FW_NAME) + "\",";
  j += "\"ver\":\"" + String(FW_VERSION) + "\",\"build\":\"" + String(FW_BUILD) + "\",";
  j += "\"chip\":\"" + chipHex() + "\",\"mac\":\"" + WiFi.macAddress() + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",\"ssid\":\"" + WiFi.SSID() + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"up\":" + String((millis() - bootMs) / 1000) + ",";
  j += "\"reset\":\"" + ESP.getResetReason() + "\",";
  j += "\"rom\":{\"latest\":\"" + String(mon.latest) + "\",\"ack\":\"" + String(mon.ack) +
       "\",\"hasNew\":" + String(hasNew() ? "true" : "false") + "},";
  j += "\"tjc\":\"" + hexOf(tjcInfo) + "\"";
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
              pageHeader("已清除") + F("<h1>凭据已清除</h1>"
                                       "<p class='h'>设备即将重启并开启配网热点（ROMWatch-…）—— 连上它、打开 "
                                       "<span style='font-family:monospace'>http://192.168.4.1/</span> 重新填 WiFi ✓</p></div></body></html>"));
  delay(600); ESP.restart();
}

void handleReboot() {
  server.send(200, "text/plain", "rebooting");
  delay(300); ESP.restart();
}

void handlePull() {
  String host = server.arg("host");
  int port = server.arg("port").toInt();
  String path = server.arg("path"); if (!path.length()) path = "/fw.bin";
  if (!host.length()) { server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing host\"}"); return; }
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"pulling\"}");
  delay(300);
  WiFiClient c;
  ESPhttpUpdate.update(c, host, port, path, "");
}

// ---------------------------------------------------------------- 配网页
/* 配网页（AP 模式，手机上来填 WiFi）—— 与主页同一套配色，同样 mobile-first ✓ */
void handlePortalRoot() {
  int n = WiFi.scanNetworks();
  String html = pageHeader("rom-watch · 配网");
  html += F("<h1>rom-watch 配网</h1><p class='h'>板子还没连上 WiFi —— 选一个网络、填密码就行 ✓</p>"
            "<div class='msg'>扫到 <b>");
  html += String(n);
  html += F("</b> 个网络。选列表里的，或在下面手填 SSID（隐藏网络用）。</div>"
            "<form method='POST' action='/save' autocomplete='off'>"
            "<label for='s'>WiFi 网络</label><select id='s' name='s'>");
  for (int i = 0; i < n; i++)
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  html += F("</select><label for='s2'>手填 SSID（可选）</label>"
            "<input id='s2' name='s2' placeholder='留空就用上面选的' spellcheck='false'>"
            "<label for='p'>密码</label><input id='p' name='p' type='password' placeholder='开放网络留空'>"
            "<button type='submit'>保存并重启</button></form>"
            "<p class='h' style='margin:14px 0 0'>保存后板子会重启，回到 <span style='font-family:monospace'>"
            "http://&lt;板子IP&gt;/</span> —— 用 <span style='font-family:monospace'>python tools\\ota.py find</span> 找回它的 IP ✓</p></div></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}
void handlePortalSave() {
  String ssid = server.arg("s2"); if (!ssid.length()) ssid = server.arg("s");
  if (!ssid.length()) {
    server.send(200, "text/html; charset=utf-8",
                pageHeader("失败") + F("<h1>SSID 是空的</h1><p class='h'>上面选一个网络，或者手填一个 SSID。</p>"
                                       "<a href='/'>← 返回重新填</a></div></body></html>"));
    return;
  }
  saveCfg(ssid, server.arg("p"));
  server.send(200, "text/html; charset=utf-8",
              pageHeader("已保存") + F("<h1>已保存，正在重启…</h1>"
                                       "<p class='h'>板子马上连新网络。万一连不上，它会自己开热点 <span style='font-family:monospace'>ROMWatch-&lt;芯片号&gt;</span>，"
                                       "连上去打开 <span style='font-family:monospace'>http://192.168.4.1/</span> 就能重新配。</p></div></body></html>"));
  delay(900); ESP.restart();
}

// ---------------------------------------------------------------- 播报 / 启动
void announce() {
  char msg[160];
  snprintf(msg, sizeof(msg), "ROMWATCH|%s|%s|%s|%s|%s|%s|%s",
           hostName().c_str(), WiFi.localIP().toString().c_str(), FW_VERSION,
           WiFi.macAddress().c_str(), WiFi.SSID().c_str(),
           mon.latest, hasNew() ? "NEW" : "ok");
  udp.beginPacket(IPAddress(255, 255, 255, 255), ANNOUNCE_PORT);
  udp.write((const uint8_t *)msg, strlen(msg));
  udp.endPacket();
  Serial.printf("[STAT] ip=%s rssi=%d latest=%s ack=%s new=%d heap=%u\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), mon.latest, mon.ack,
                hasNew(), ESP.getFreeHeap());
}

void startSTA() {
  apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostName());
  /* 2026-09-26：这里原来提前调了 clockStart() ✗ —— 那时还没连上网，SNTP 解析域名偶发崩溃 ✗
   * （黑匣子实测：连WiFi阶段 Exception ✓）⇒ 挪到连上之后再调 ✓，配合 loop 里 30 秒重试 ✓ 一样能校上 ✓ */
  /* ★★2026-09-26 晚：主人给的 WiFi 凭据【写死】⇒ 一劳永逸 ✓
   * ★2026-09-29 改：**值挪到 config.h**（PVT_WIFI_SSID/PASS）—— 
   *   行为一模一样（开机比对，不一样就覆盖写进 EEPROM ✓），
   *   但主文件里再也不含真实凭据 ⇒ 将来要分享/开源只换 config.h 一个文件 ✓✓
   *   （config.h 里留空则整段跳过 ⇒ 走 AP 配网 ✓） */
#if PVT_WIFI_SET
  if (strcmp(cfg.ssid, PVT_WIFI_SSID) != 0 || strcmp(cfg.pass, PVT_WIFI_PASS) != 0) {
    saveCfg(PVT_WIFI_SSID, PVT_WIFI_PASS);
    Serial.printf("[WIFI] 已写入 config.h 里的凭据: SSID=%s（旧的是 '%s'）\n", PVT_WIFI_SSID, cfg.ssid);
  } else {
    Serial.printf("[WIFI] 凭据已是: SSID=%s ✓\n", cfg.ssid);
  }
#endif
  applyStaticIp();                           // ★静态 IP 必须在 WiFi.begin() **之前** config ✓（DHCP 时它直接返回 ✓）
  WiFi.begin(cfg.ssid, cfg.pass);
  bbPhase(PH_WIFI);
  unsigned long t0 = millis();
  /* ★★2026-09-26 修正：连不上【不再一次就躲进配网模式 ✗】
   * 起因：屏接线后板子掉过一次线 ⇒ 一次 25 秒没连上就进 AP ⇒ 网上再也找不到它 ✗
   *       ⇒ 本喵只能靠拔线/串口救，没法远程修 ✗
   * 改成：连试 3 轮（每轮 25 秒 ✓），都失败才进 AP ✓
   *       ⇒ 偶发失败能自愈 ✓，而且【优先保证回到原来的网】✓ 本喵能远程继续修 ✓✓
   */
  for (uint8_t round = 1; round <= 3 && WiFi.status() != WL_CONNECTED; round++) {
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) { ledOn(); delay(120); ledOff(); delay(130); }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[WIFI] 第 %u 轮失败，重试…\n", round);
      WiFi.disconnect(); delay(500);
      WiFi.begin(cfg.ssid, cfg.pass);
    }
  }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[WIFI] 失败 -> 配网模式"); startAP(); return; }

  bb.wifiMs = (uint16_t)(millis() - t0); bb.net = 1; bbSave();
  Serial.printf("[WIFI] OK ip=%s\n", WiFi.localIP().toString().c_str());
  bbPhase(PH_NTP);
  clockStart();                              // ★连上之后再校时（安全 ✓）

  bbPhase(PH_SERVER);
  httpUpdater.setup(&server, "/update", "admin", OTA_PASSWORD);
  server.on("/", handleRoot);
  server.on("/rom", handleRom);
  server.on("/status", handleStatus);
  server.on("/ping", handlePing);
  server.on("/reset", handleReset);
  server.on("/reboot", handleReboot);
  server.on("/pull", handlePull);
  server.on("/check", HTTP_POST, handleCheck);
  server.on("/led", handleLed);              // ★/led?on=1|0|2  手动点灯（把"灯坏没坏"和"逻辑"分开 ✓）
  server.on("/seen", HTTP_POST, handleSeen); // ★/seen  "我知道了（关灯）"——不改语义、只灭灯 ✓
  server.on("/ack", HTTP_POST, handleAck);
  server.on("/skip", HTTP_POST, handleSkip);
  server.on("/cfg", HTTP_POST, handleCfg);
  server.on("/tjc", handleTjc);
  server.on("/tjccmd", handleTjcCmd);       // ★新增：浏览器直接把一条 TJC 指令发给屏（调试用 ✓）
  server.on("/i2c", handleI2cScan);
  server.on("/reconfig", handleReconfig);
  server.on("/dl", handleDl);
  server.on("/log", handleLog);
  server.on("/tjcsweep", handleTjcSweep);
  server.on("/tjcwire", handleTjcWire);
  server.on("/dlclear", handleDlClear);
  server.begin();

  bbPhase(PH_OTA);
  ArduinoOTA.setHostname(hostName().c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { otaRunning = true; ledOn(); });
  ArduinoOTA.onEnd([]() { otaRunning = false; });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    static int last = -1; int pct = t ? (int)(p * 100 / t) : 0;
    if (pct / 20 != last / 20) { last = pct; Serial.printf("[OTA] %d%%\n", pct); }
  });
  ArduinoOTA.onError([](ota_error_t e) { otaRunning = false; Serial.printf("[OTA] err %u\n", e); });
  ArduinoOTA.begin();

  bbPhase(PH_DEV);
  udp.begin(ANNOUNCE_PORT);
#if USE_TJC
  /* ★★2026-09-26 修正：开机【自动探测屏的波特率】✓
   * 起因：工程 Program.s 里写的是 baud=9600 ✗，而这里原来固定 115200 ✗
   *       ⇒ ESP 发过去的字屏一个都听不懂 ⇒ 回包恒为 0 字节 ✗
   * 做法：依次试几档，谁回 comok 就用谁 ✓；探到的值记在 tjcBaud 里、/log 与 /rom 都能看到 ✓
   * ★注意：SoftwareSerial 换波特率必须 end() 再 begin() ✓ */
  {
    const long tryBauds[] = { 115200, 9600, 19200, 57600, 38400, 230400 };
    const uint8_t nBauds = sizeof(tryBauds) / sizeof(tryBauds[0]);
    for (uint8_t bi = 0; bi < nBauds && tjcInfo.length() == 0; bi++) {
      tjc.end();
      tjc.begin(tryBauds[bi]);
      delay(250);
      tjcReady = true;
      String r = tjcCmd("connect", 500);
      if (r.length()) {
        tjcBaud = tryBauds[bi];
        tjcInfo = r;
        Serial.printf("[TJC] 屏在 %ld 上应答 ✓\n", (long)tjcBaud);
      } else {
        tjcReady = false;
        Serial.printf("[TJC] %ld 无应答\n", (long)tryBauds[bi]);
      }
    }
    if (tjcInfo.length() == 0) {          // 全都不回 ⇒ 回到首选档，留着以后重试 ✓
      tjc.end(); tjc.begin(115200); delay(150);
      tjcBaud = 115200;
    }
    tjcReady = true;
  }
  delay(150);
#else
  Serial.println("[TJC] 本固件编译时未启用串口屏（USE_TJC=0）⇒ 跳过屏的探测 ✓");
#endif
  /* OLED 已在 setup 里提前初始化（为了能在连网前就显示自检 ✓），这里只补一次刷新 */
  stNet = true;
  stDraw();
  delay(600);
  Serial.println("[I2C] SDA=D2 SCL=D1 已初始化，扫描结果见 /i2c");
#if USE_TJC
  Serial.printf("[TJC] connect -> %s\n", hexOf(tjcInfo).c_str());
  /* ★开机打开【触摸坐标上报】✓ —— 屏重启后 sendxy 会重置回 0 ✗ ⇒ 每次开机都要发一遍 ✓
     有了它，本喵就【不需要给任何控件绑事件】✓✓（见 tjcTouch ✓）*/
  tjcSend("sendxy=1");
  delay(120);
#endif
  announce(); lastAnnounce = millis();
  Serial.printf("\n[BOOT] %s %s build %s  http://%s/\n\n",
                FW_NAME, FW_VERSION, FW_BUILD, WiFi.localIP().toString().c_str());
  /* ★2026-09-26：原来在这里**立刻** doCheck(true) ✗ —— TLS 握手要一大块连续堆，
   * 而刚开完服务器/OTA/UDP，堆正紧（黑匣子实测：阶段 7 Exception ✗）。
   * 改成**推迟 15 秒**、在主循环里做 ✓：那时系统已稳、也能先响应网页 ✓。 */
  bootCheckDue = millis() + 15000UL;
}

void startAP() {
  apMode = true;
  String apSsid = String(AP_PREFIX) + chipHex();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str());
  delay(300);
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", handlePortalRoot);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.onNotFound([]() { handlePortalRoot(); });
  server.begin();
  Serial.printf("\n[AP] 配网热点：%s  http://%s/\n\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void setup() {
  bbLoad();                                  // ★先把上次的崩溃信息读出来（RTC 内存 ✓）
  bbPhase(PH_SETUP);
  pinMode(LED_PIN, OUTPUT); ledOff();
  pinMode(BTN_PIN, INPUT_PULLUP);         // 板载 FLASH 键（GPIO0）

  /* ① 先把屏点亮（连网前就能看到自检 ✓）—— 没有 OLED 时这几行不编 ✓ */
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);   // 总线照旧初始化（/i2c 扫描要用 ✓）引脚走适配层 ✓
  delay(50);
#if USE_OLED
  oled.init();
  oled.flipScreenVertically();
#endif
  Serial.begin(115200); delay(150);
  bootMs = millis();
  Serial.printf("\n\n=== %s %s build %s (chip %s) ===\n", FW_NAME, FW_VERSION, FW_BUILD, chipHex().c_str());
  EEPROM.begin(1024);                      // ① 扩到 1024：512 起放历史表 ✓（0~511 原样保留 ✓）
  cfg.magic = 0;
  EEPROM.get(CFG_OFF, cfg);
  monInit();                                 // 先读监测状态（配网模式也要能显示）
  histLoad();                                // ① 读检测历史（EEPROM 512 起 ✓）
  netLoad();                                 // ★串口屏的网络配置（EEPROM 768 起、独立 magic ✓ 老布局不动 ✓）
  extLoad();                                 // ★数据源预设/标题（EEPROM 800 起、独立 magic ✓）
  bbPhase(PH_SELFTEST);
  bootSelfTest();
  bb.oled = stOled; bb.i2c = stI2c; bb.btn = stBtn; bb.cfg = stCfg;
  bbSave();                                  // 自检结果进黑匣子 ✓（哪一项不过一目了然 ✓）                           // ② 开机自检（此刻配置已读入 ✓、还没连 WiFi ✓）
  if (cfg.magic != CFG_MAGIC || cfg.ssid[0] == 0) { memset(&cfg, 0, sizeof(cfg)); startAP(); }
  else startSTA();
  lastPoll = millis();
}

void prefixLedSolid() {                    // 报警灯：常亮（不做任何翻转，避免"一直闪"）
  /* ★2026-09-29 修（本喵自己埋的 bug ✗）：这里原来是 `static bool on` 闩锁 ——
   *   "以为灯还亮着就不写了" ✗ ⇒ 一旦别处把灯关了（摁「我知道了」/ 手动 `/led?on=0`），
   *   闩锁还记着 on=true ⇒ 再想报警时**灯永远不亮** ✗✗（实测：撤销"我知道了"之后 ledAlarm=true 但引脚还是 HIGH ✓）。
   *   ⇒ 现在**每次直接写 LOW** ✓ —— digitalWrite 同一个电平不会闪，也不怕别人改过灯 ✓ */
  ledOn();
}

void handleButton() {
  /* 板载 FLASH 键（GPIO0）手势（2026-09-26 重做，主人反馈"双击不灵敏"）：
   *   短按一下      -> 我已刷（立刻显示 ✓）
   *   按住 1.5~3 秒 -> 松手 = 跳过这版
   *   按住 >= 3 秒  -> 请求下载（★唯一的下载触发方式 ✓ —— 2026-09-26 去掉"双击"：
   *                    主人反馈"我点两下就直接请求下载了"✗ ⇒ 随手两下就弹 Edge，太容易误触 ✗）
   * 注：板上那个 RST 键是**硬复位**，按了就重启，读不到，不能当按钮用 ✗。 */
  static bool          last = HIGH;
  static unsigned long downAt = 0;
  static bool          longFired = false;
  bool now = digitalRead(BTN_PIN);

  if (last == HIGH && now == LOW) {           // 按下
    downAt = millis();
    longFired = false;
  } else if (now == LOW && !longFired && millis() - downAt > 3000UL) {
    longFired = true;                          // ★按住 3 秒：请求下载（最可靠 ✓）
    dlReq = String(mon.latest);
    Serial.println("[BTN] 长按3秒 → 请求下载 " + dlReq);
    oledBanner("已请求下载");
    oledUpdate();
  } else if (last == LOW && now == HIGH) {     // 松开
    unsigned long held = millis() - downAt;
    if (longFired) {
      /* 已经在 3 秒那一步处理过了 ✓ */
    } else if (held >= 1500UL) {               // 1.5~3 秒：跳过这版 ✓
      Serial.println("[BTN] 长按 → 跳过 " + String(mon.latest));
      oledBanner("已跳过");
      doSkip();                                // ★与网页 /skip、屏 #SKIP 同一份实现 ✓
    } else if (held > 40UL) {                  // 短按 = 我已刷（立刻 ✓，点两下也只是确认两次 ✓ 不会下载 ✓）
      Serial.println("[BTN] 短按 → 已刷 " + String(mon.latest));
      oledBanner("我已刷");
      doAck();                                 // ★与网页 /ack、屏 #ACK 同一份实现 ✓
    }
  }
  last = now;
}

void loop() {
  if (bb.phase != PH_LOOP) { bb.phase = PH_LOOP; bb.lastResetMs = millis(); bbSave(); }
  if (apMode) {
    dns.processNextRequest();
    server.handleClient();
    static unsigned long t = 0;
    if (millis() - t > 150) { t = millis(); digitalWrite(LED_PIN, !digitalRead(LED_PIN)); }
    return;
  }

  ArduinoOTA.handle();
  server.handleClient();

  /* ★网页【立即检查】：HTTP 里只置标志（秒回 ✓），真正的抓包在这里做 —— 
   *   顺带避开"在 HTTP 回调里做 25 秒的 TLS 握手"（那会把整个服务器卡死 ✗）*/
  if (checkSoon) { checkSoon = false; if (!checking) doCheck(true); }

  if (millis() - lastAnnounce > 30000UL) { announce(); lastAnnounce = millis(); }
  /* ② 失败快速重试：失败后按 60s×失败次数 重试（上限=正常间隔），不用干等 10 分钟 ✓ */
  unsigned long waitS = waitSecs();          // ★与 /rom 的"下次检查"共用同一份计算 ✓
  if (millis() - lastPoll > waitS * 1000UL) { lastPoll = millis(); doCheck(false); }

  if (!otaRunning && millis() >= ledForceUntil) {   // ★手动点灯期间不抢（见 /led ✓）
    /* 2026-09-25 主人要求：灯不要一直闪 ✗ ⇒ 报警＝常亮（一眼看得见且不烦）✓；正常＝全灭 ✓
     * ★2026-09-29：改看 ledAlarm() ⇒ 摁过"我知道了"就灭，但**页面仍显示有新版本** ✓✓ */
    if (ledAlarm()) prefixLedSolid();
    else            ledOff();
  }

  handleButton();                            // 板载 FLASH 键：短按=我已刷，长按≥1.5s=跳过

  /* ★串口屏：非阻塞收报（每圈只吃"已经到"的字节 ✓）+ 状态变化才推（3 秒检查一次节流 ✓）
   * ★2026-09-29：整段进开关（=0 时连"每 3 秒空转一次"都不要 ✓ 收报缓冲也在里面 ✓）*/
#if USE_TJC
  tjcPoll();
  if (millis() - lastTjcPush > 3000UL) {
    lastTjcPush = millis();
    tjcPush(false);                          // 内部比对指纹：没变就一个字都不发 ✓
  }
#endif

  /* ★2026-09-30 修（主人报「上面的时间差了一分钟」✗）：
   *   老写法＝「开机后**每 60 秒**重画一次」✗ —— 这个周期**不跟"整分钟"对齐** ✗：
   *   假设开机落在 :40，屏上的分钟数就只在每分钟的 :40 才换 ⇒ **从 :00 到 :40 屏上一直停在上一分钟** ✗✗
   *   （也就是每分钟里有 40 秒看着"慢了整整 1 分钟"✓ —— 主人看到的正是这个）。
   *   实测：板子的时间本身是准的（`/rom` 的 now 与电脑只差 0.9 秒 ✓）⇒ 问题**只在"什么时候把新分钟画上去"** ✓。
   *   现在：**盯"分钟数变了没"**（每分钟的 :00 一变就立刻重画 ✓）；
   *   还没校好时（屏上显示 12s/5m/1h 那种"距今多久"）仍按 60 秒刷一次 ✓。
   *   代价：每次 loop 多一次 `time()`（纯读 RTC，微秒级 ✓）。*/
  if (!otaRunning && !checking) {
    if (clockReady()) {
      time_t tNow = time(nullptr);
      if (tNow / 60 != lastShownMin) { lastShownMin = tNow / 60; lastScreenAt = millis(); oledUpdate(); }
    } else if (millis() - lastScreenTick > 60000UL) {
      lastScreenTick = millis();
      lastScreenAt = millis();
      oledUpdate();
    }
  }

  /* ★开机那次抓包：推迟到这里做（躲开刚开完服务器时堆最紧的时刻 ✓） */
  if (bootCheckDue && millis() > bootCheckDue) {
    bootCheckDue = 0;
    bbPhase(PH_CHECK);
    doCheck(true);
  }

  /* ★校时没成功就每 30 秒重试一次（DNS/网络抖一下也能自己恢复 ✓）
   * ★加"必须已联网"这一条：硬约束#2 说得很明白 —— 没连上就调 configTime/clockStart 会偶发崩 ✗。
   *   原来这里没判连接状态：屏上 #WIFI 换网失败 / 掉线期间，每 30 秒就会在**离线状态**下发一次 SNTP ✗ */
  if (!clockReady() && WiFi.status() == WL_CONNECTED && millis() - lastNtpTry > 30000UL) {
    lastNtpTry = millis();
    clockStart();
    Serial.println("[NTP] 还没校上，重试…");
  }

  static unsigned long lastRe = 0;
  if (millis() - lastRe > 10000) {
    lastRe = millis();
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[WIFI] 掉线重连"); WiFi.reconnect(); }
  }
}
