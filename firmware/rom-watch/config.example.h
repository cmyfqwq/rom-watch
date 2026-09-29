/*
 * config.example.h —— rom-watch 的**配置样板**（2026-09-30 加）
 * ============================================================================
 * 用法：把本文件复制成 `config.h`，改成你自己的值即可 ✓
 *     copy config.example.h config.h        （Windows）
 *     cp   config.example.h config.h        （Linux/macOS）
 *
 * ★为什么要有这个文件：主文件 rom-watch.ino 用 `#if __has_include("config.h")` 探测它 ⇒
 *   ① **没有 config.h 也能编译**（走中性默认值 + 开机 AP 配网 ✓）
 *   ② 真实的 `config.h` 里是你的 WiFi 密码和服务器地址 ⇒ 它**永远不进仓库**
 *      （`.gitignore` 里已经排掉了 `firmware/rom-watch/config.h` ✓）
 *   ③ 所以仓库里只放这一份"空壳样板" ✓
 * ============================================================================
 */
#pragma once

/* ===== ① 编译期功能开关 =====================================================
 * 写成 #ifndef ⇒ 也可以**从命令行覆盖**（不用改本文件）：
 *     arduino-cli compile ... --build-property "compiler.cpp.extra_flags=-DUSE_TJC=0"
 *   ⇒ 多配置编译矩阵就靠这个（tools\build_matrix.py ✓）
 * 用途：IRAM 只有 64KB，全开是 96% ⇒ 不用哪个就把哪个关掉 ✓ */
#ifndef USE_TJC
#define USE_TJC    1        // 串口屏（SoftwareSerial + 坐标驱动 + 状态推送），最吃 IRAM
#endif
#ifndef USE_OLED
#define USE_OLED   1        // 0.96" OLED（含中文字模，吃 flash 不吃 IRAM）
#endif

/* ===== ② 你的私有默认值（首次烧录 / EEPROM 被清时用它）=====================
 * ★PVT_WIFI_SET 是个**数字**开关，必须这样写 —— 预处理器的 #if **不能对字符串字面量取下标**
 *   （`#if PVT_WIFI_SSID[0]` 会报 `token ""x"" is not valid in preprocessor expressions` ✗，实测）
 *   1 = 开机强制写入下面这套 WiFi 凭据（"写死一劳永逸"）
 *   0 = 不写，开机走 AP 配网（★**要分享/开源就用 0** ✓）*/
#define PVT_WIFI_SET   0
#define PVT_WIFI_SSID  "你的WiFi名"                 // 2.4G
#define PVT_WIFI_PASS  "你的WiFi密码"

/* 默认数据源（网页「监测设置」里随时能改，这里只是"第一次开机"的初值）
 * ★留空 = 还没配：固件会直接给一句人话（"还没配数据源：网页「监测设置」里填" ✓），
 *   不会拿着空主机名去握手 ✓ —— 所以**空着是完全合法的默认值** ✓ */
#define DFLT_HOST      ""                          // 例：alist.example.com
#define DFLT_PATH      ""                          // 例：/public/roms
#define DFLT_INTERVAL  600                         // 轮询秒（网页最小 60）

/* ===== ③ 硬件适配（接线 / 显示屏 / 按键）—— 全是可选的 ======================
 * 一条都不写就走主文件里的默认值（WeMos D1 mini 标准接线 ✓）
 * ⇒ 换板子 / 换屏 / 换接线时，**只在这个文件里加几行**，主文件一个字都不用动 ✓✓
 *
 *   #define PIN_OLED_SDA  D2           // I2C 数据（默认 D2）
 *   #define PIN_OLED_SCL  D1           // I2C 时钟（默认 D1）
 *   #define OLED_ADDR     0x3C         // 少数模块是 0x3D
 *   #define OLED_SMALL    1            // 128x32 的小屏（0.91"）—— 版面自动变两行 ✓
 *   #define PIN_TJC_RX    D5           // 串口屏 TX → 板 D5
 *   #define PIN_TJC_TX    D7           // 串口屏 RX → 板 D7（★别用 D6：开机判 flash 电压）
 *   #define PIN_LED       LED_BUILTIN  // 板载 LED（低电平点亮）
 *   #define PIN_BTN       D3           // 板载 FLASH 键（按下为低）
 *   #define OTA_PASSWORD  "换成你自己的"  // ★分享/开源前**务必**改掉默认口令 ✓
 *                                        //   （`tools/ota.py` 会**自动读 `config.h` 里这一行** ⇒
 *                                        //    口令只有一个真值源，不用再去改脚本 ✓；
 *                                        //    环境变量 `ROMWATCH_OTA_PASS` 优先级更高，可临时顶替 ✓）
 * ============================================================================ */
