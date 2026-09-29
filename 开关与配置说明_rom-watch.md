# rom-watch 开关与配置说明（给自己看的速查）

> 2026-09-29 · 目的：**不用翻 2000 行代码也能知道"哪儿改、改了会怎样"**
> 三个文件的分工：`config.h`（**你的私有配置**）· `rom-watch.ino`（业务）· `web_ui.h`（网页）
> 当前版本 **0.3.1**（编译开关 + 数据源通用化 + 网页大屏/看板 + 标题可配）

---

## 0. 现在能盯哪些东西？（数据源预设）

网页「监测设置 → 数据源」里选，或在 `/cfg` 里传 `preset=`：

| 预设 | 干什么 | 服务器 | 路径 | 取值/时间键 | 方式 |
|---|---|---|---|---|---|
| `0` **OpenList 目录**（默认） | 网盘某个目录里有没有新版本 | 你的服务器（如 `alist.example.com`） | 目录（如 `/你的目录/…`） | `name` / `created` | POST |
| `1` **GitHub Release** | 某个仓库的最新发行版 | `api.github.com` | `/repos/OWNER/REPO/releases/latest` | `tag_name` / `published_at` | GET |
| `2` **自定义 JSON** | 任意接口 | 自己填 | **就是请求路径本身** | 自己填 | GET/POST |

**实测（2026-09-29 真机，非推测）**：切到 GitHub 预设后板子抓到 `3.1.2` / `2023-03-20T22:25:05`；
再切回 OpenList 抓回 `v4.0.20` ⇒ 八字段原样还原 ✓（脚本 `qwq\_esp_source_test.cjs`，含还原断言）。

**★★两个必须记住的坑**：
1. **"请求路径" ≠ "要盯的目标"** —— OpenList 的请求路径**恒定**是 `/api/fs/list`，`mon.path`（目录）走 **POST body** ✓。
   本喵一开始把两者混为一谈 ⇒ POST 打到了目录上 ⇒ 服务器回**网页 HTML** ⇒ 报 `API 非 200`
   （现象特别像"服务器挂了"，其实是自己打歪了 ✗）。**只有 preset 2（自定义）才把"路径"当请求路径** ✓。
2. **GitHub 两条硬规矩**：必须带 `User-Agent`（不带直接 403）；未登录**限流 60 次/小时/IP** ⇒ 轮询别小于 10 分钟 ✓。

---

## 1. 编译开关（`config.h` 最上面两个）

| 开关 | 含义 | 关掉会怎样 |
|---|---|---|
| `USE_TJC` | 串口屏（SoftwareSerial D5/D7 + 坐标驱动 + 状态推送 + `/tjc*` 端点） | 屏相关代码**整块不编** ⇒ IRAM 省一大块；`/tjc` `/tjccmd` `/tjcsweep` `/tjcwire` 会如实回「本固件未启用串口屏」而不是 404 |
| `USE_OLED` | 0.96" OLED（含中文点阵 `cn_font.h`） | 自检页/状态页/横幅全变空动作 ⇒ flash 省 ~250KB；`/i2c` 扫描照旧可用 |

**改法有两种**（推荐命令行，省得动 `config.h` 把 WiFi 凭据搞丢）：

```powershell
# ① 改文件：把 config.h 里的 1 改成 0
# ② 命令行覆盖（不改文件 ✓）：
cd <仓库>\serial-screen
python tools\ota.py build --sketch rom-watch
& tools\arduino-cli\arduino-cli.exe --config-file tools\arduino15\arduino-cli.yaml compile `
  --fqbn esp8266:esp8266:d1_mini --output-dir firmware\build-matrix\minimal `
  --build-property "compiler.cpp.extra_flags=-DUSE_TJC=0 -DUSE_OLED=0" firmware\rom-watch
```

**一键跑四种配置的对比表**：`python tools\build_matrix.py`（产物在 `firmware\build-matrix\<配置名>\`，**不碰** `build\` 里给 OTA 用的 bin ✓）

实测资源占用（`python tools\build_matrix.py` 输出，2026-09-30 六配置）：

| 配置 | 编译开关 | RAM | IRAM | Flash | bin |
|---|---|---|---|---|---|
| full | 默认 | 39996 (49%) | 63475 (96%) | 637172 (60%) | 683040 B |
| no_tjc | `-DUSE_TJC=0` | 37400 (46%) | 61923 (94%) | 623260 (59%) | 665568 B |
| no_oled | `-DUSE_OLED=0` | 39784 (49%) | 63475 (96%) | 524764 (50%) | 570528 B |
| minimal | 两个都 0 | 37144 (46%) | 61923 (94%) | 510852 (48%) | 553008 B |
| oled32 | `-DOLED_SMALL=1` | 39976 (49%) | 63475 (96%) | 636532 (60%) | 682384 B |
| **portable** | **干净副本（不带 `config.h`）** | 39820 (49%) | 63475 (96%) | 637060 (60%) | 682752 B |

**★`portable` 那一行是"能开源"的实测证据**：脚本把 `rom-watch.ino` / `web_ui.h` / `cn_font.h` 三个可公开文件复制到临时目录、
**故意不带 `config.h`** 编一遍 ⇒ 编过了 ⇒ 别人 clone 下来直接就能编译 ✓✓（光在注释里写"能编"不算证据 ✗）。

**★怎么读这张表（不吹）**：
- `USE_TJC=0` ⇒ **IRAM 只降 2 个点（−1552 B）**，flash 省 13 KB，RAM 省 2.5 KB。**它没"解决"IRAM 问题** ✗ ——
  因为剩下那 94% 是 WiFi + BearSSL + WebServer + ArduinoOTA 这些**底座**，关不掉。
- `USE_OLED=0` ⇒ IRAM 一点不降（本来就不吃 IRAM），但 **flash 省 73 KB**（中文字模是大头）。
- ⇒ **结论**：这块板子的 IRAM 天花板就在 94~96% 之间 ⇒
  ①"没有屏/没有 OLED 也能编"这个目的是达成的 ✓（顺带省 flash 与 RAM ✓）
  ②但**想再加功能**只能靠：把字符串塞 PROGMEM、少用 `String` 拼接、必要时关 mDNS，或者**换 ESP32-C3**（内存大 4~8 倍）。别指望关开关能腾出空间 ✗

---

## 2. `config.h` 里都有什么

| 宏 | 作用 | 注意 |
|---|---|---|
| `USE_TJC` / `USE_OLED` | 见上 | 都是 `#ifndef` 包着 ⇒ 命令行 `-D` 可覆盖 |
| `PVT_WIFI_SET` | 开机要不要**强制写入**下面那套 WiFi | `1` = 写（你现在的行为）；`0` = 不写，走 AP 配网 |
| `PVT_WIFI_SSID` / `PVT_WIFI_PASS` | 你的 WiFi（2.4G） | ⚠️ **明文**，这份文件就是"私有文件"，别外发 |
| `DFLT_HOST` / `DFLT_PATH` | 监测源默认值（EEPROM 被清时用） | 网页上也能随时改；**留空是合法的**（固件会提示"还没配数据源" ✓） |
| `DFLT_INTERVAL` | 轮询默认秒数 | 最小 60 秒 |

**★硬件适配（③ 节，全是可选的 —— 2026-09-30 加）**：换板子/换屏/换接线时**只改 `config.h`**，主文件一个字不动 ✓

| 宏 | 默认 | 作用 |
|---|---|---|
| `PIN_OLED_SDA` / `PIN_OLED_SCL` | `D2` / `D1` | I2C 引脚 |
| `OLED_ADDR` | `0x3C` | 少数模块是 `0x3D` |
| `OLED_SMALL` | `0` | 设 `1` ⇒ 128×32 小屏（0.91"），**版面自动换成两行** ✓ |
| `PIN_TJC_RX` / `PIN_TJC_TX` | `D5` / `D7` | 串口屏软串口（★别用 D6：开机判 flash 电压） |
| `PIN_LED` / `PIN_BTN` | `LED_BUILTIN` / `D3` | 板载 LED / FLASH 键 |
| `OTA_PASSWORD` | `meow-ota-8266` | ★分享/开源前**务必**改掉 |

板型也可换（不用改脚本，用环境变量）：`set ROMWATCH_FQBN=esp8266:esp8266:nodemcuv2`
OTA 口令/串口同理：`ROMWATCH_OTA_PASS` / `ROMWATCH_PORT` ✓

**★配置样板**：仓库里只放 `config.example.h`（空壳）；真实的 `config.h`（带 WiFi 密码）**永不入库**（`.gitignore` 已排掉 ✓）。

⚠️ **坑（我踩过）**：`#if PVT_WIFI_SSID[0]` 这种写法**编译不过** —— 预处理器的 `#if` 不能对字符串字面量取下标（会报 `token ""你的SSID"" is not valid in preprocessor expressions`）⇒ 所以要有 `PVT_WIFI_SET` 这个**数字**开关。

---

## 3. EEPROM 布局（1KB，**这是红线，别乱动**）

| 偏移 | 内容 | 结构 | 谁在用 |
|---|---|---|---|
| 0 | WiFi 凭据 | `Cfg`（magic=`OTA1`） | 与 `ota-base` 底座**同布局** ⇒ 换固件不用重配 WiFi ✓ |
| 128 | 监测状态 | `Mon`（magic=`ROM1`） | 间隔 / host / path / 水位线 ack / 跳过 skip / 最新版 / 失败计数 |
| 512 | 最近 6 次检测 | `Hist`（magic=**`HIS2`**） | 网页"最近 6 次检测"表（★每条带 `src`＝当时在盯哪个源 ✓） |
| 768 | 网络配置 | `NetCfg`（magic=`NET2`） | 静态 IP / DHCP |
| 800 | `Ext` 数据源预设 + 标题 + 通知（magic=**`EXT4`**） | `preset` / `method` / `jsonKey[16]` / `timeKey[16]` / `title[24]` / `webBase[64]` / `body[32]` / **`seen[24]` / `found[32]` / `foundUp`** | 数据源预设、页面标题、网页基址、「我知道了（关灯）」的 seen、「首次发现于 X」 |
| 1024 | — | | `EEPROM.begin(1024)` |

**规矩**：① 现有结构**一个字节都不许挪**（挪了 = 你的 WiFi/水位线全丢）② 新东西一律放 800 起、**换结构就换 magic**（老数据自动作废，绝不会把垃圾字节当配置读进来）✓
③ `Ext` 在 800 起占 216 字节 ⇒ 800~1015，还剩 ~8 字节（**下次加字段要换 magic 并挪到新位置或压缩现有字段** ✓）

---

## 4. 常用端点（**注意：动作类端点都是 POST**，GET 会 404）

| 端点 | 方法 | 干什么 |
|---|---|---|
| `/` | GET | 网页看板（手机/电脑/电视都能开） |
| `/rom` | GET | 状态 JSON（网页每 5 秒拉一次；也方便脚本读）★含 `led`（**回读引脚真实电平**）/`ledAlarm`/`seen`/`found`/`scrMin`（屏上画的是第几分钟）/`scrUp` |
| `/status` `/ping` `/log` | GET | 设备信息 / 存活探测 / 纯文本日志 |
| `/ack` `/skip` `/check` | **POST** | 我已刷 / 跳过这版 / 立即检查（`/check` 0.3.0 起秒回 ✓） |
| `/seen` | **POST** | ★**「我知道了（关灯）」** —— **不动水位线**、页面照旧显示"有新版本"，只把灯摁灭；`?clear=1` 撤销 ✓ |
| `/led?on=1\|0\|2` | GET | ★手动点灯 15 秒 —— 把"**灯坏没坏**"和"**逻辑要不要亮**"一刀切开 ✓ |
| `/cfg` | **POST** | 改 path / host / interval / ack |
| `/dl` `/dlclear` | POST/GET | 请求电脑端下载 / 清标志 |
| `/tjccmd?c=<TJC指令>` | GET | 隔着 WiFi 给屏发一条指令并看回包（调试神器） |
| `/tjcwire` | GET | **D5–D7 短接**打方波，把"板子/线的问题"和"屏的问题"切开 |
| `/tjcsweep` | GET | 逐档试屏的波特率 |
| `/i2c` | GET | 扫 I2C 总线（看 OLED 在不在，0x3C） |
| `/update` | GET | 网页刷固件（用户名 `admin` / 口令＝`OTA_PASSWORD`，**默认口令请务必改掉** ✓） |
| `/reset` `/reconfig` `/reboot` | POST | 清 WiFi 凭据 / 重新配网 / 重启 |

---

## 5. 出问题时的对号入座

| 现象 | 先看这里 |
|---|---|
| 网页打不开 | 板子还在不在网：`python tools\ota.py find`；串口日志：`python tools\ota.py serial` |
| 屏上没反应 / 字段不变 | ①`/rom` 里 `tjcOk` 是不是 false ②`/tjcwire`（短接 D5-D7）分清板子和屏 ③`/tjcsweep` 试波特率 ④**屏的 sendxy 每次上电都会重置**，固件开机自动重发 ✓ |
| 编译报 IRAM 超了 | `python tools\build_matrix.py` 看哪个开关能关；或砍功能 |
| 板子反复重启 | `/log` 看「上次跑到阶段」＋启动次数（黑匣子，RTC 内存） |
| 抓到的时间不对 | `/log` 看「校时」；没 NTP 时显示的是"开机计时" |
| 网页数字不刷新 | 页面每 5 秒拉 `/rom`；按 F12 看是不是 fetch 失败（跨网段/代理） |

---

## 6. 一键命令速查

```powershell
cd <仓库>\serial-screen
python tools\ota.py push --sketch rom-watch     # 编译 + OTA 推 + 自动校验（日常就这条）
python tools\ota.py status                      # 看板子 /status
python tools\ota.py serial --sec 20             # 看串口日志
python tools\build_matrix.py                    # 多配置资源对比
pwsh -File qwq\screen-flash.ps1 -Hmi <工程.HMI>  # 刷串口屏工程（全自动）
python serial-screen\tools\verify_hmi.py <工程.HMI>  # 屏工程自检
node qwq\_esp_preview.cjs                       # 网页本地预览出图（假的 /rom 数据）
node qwq\_esp_verify.cjs                        # 真机验收：连拉整页测堆 + DOM 断言 + 截图
```
