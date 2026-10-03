/*
 * config.example.h —— ota32-base 配置样板
 * ============================================================================
 * 用法：复制成 config.h 再填你的值 ⇒ ★config.h（真凭据）永远不进仓库 ✓
 *       （tools/ota.py 会自动读 config.h 里的 OTA_PASSWORD ✓）
 */
#ifndef PVT_WIFI_SET
#define PVT_WIFI_SET 0              // 1 = 把下面的 WiFi 编进固件（开机直连）✓
                                    // 0 = 留空 ⇒ 首次开机起热点 OTA32Base-xxxxxx 配网 ✓
#endif
#if PVT_WIFI_SET
#define PVT_WIFI_SSID "你的WiFi名"    // ★只支持 2.4G（这块芯片没有 5G ✗）
#define PVT_WIFI_PASS "你的WiFi密码"  // ★大小写敏感 ✓
#endif

#define OTA_PASSWORD "meow-ota-s3"  // ★★必须改：同网段的任何人都能刷你的板子 ✗
