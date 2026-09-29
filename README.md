# rom-watch

**一块 15 块钱的 ESP8266，替掉「每天手动点开网盘看一眼有没有新版本」这件破事。**

![version](https://img.shields.io/badge/version-1.0.0-blue?style=flat-square)
![license](https://img.shields.io/badge/license-MIT-green?style=flat-square)
![platform](https://img.shields.io/badge/platform-ESP8266%20%2F%20Arduino-orange?style=flat-square)
![deps](https://img.shields.io/badge/dependencies-none-brightgreen?style=flat-square)

> 有新版本就 **亮灯 + 屏上换字 + 网页变红**；刷完点一下「我已刷 ✓」，它就安静下来。
>
> 🇨🇳 **Gitee 镜像（国内访问更快）**：<https://gitee.com/cmyfqwq/rom-watch>
> 两边内容一致，Issue 提哪边都行（主仓库是 GitHub）

---

## 30 秒看懂

| | |
|---|---|
| **盯什么** | 网盘目录（OpenList / alist）· GitHub Release · 任意 JSON 接口 —— 网页上点一下就能换 |
| **怎么提醒** | 板上 LED 常亮 ＋ 0.96" OLED 换字 ＋ 网页卡片变红（**三处同时**） |
| **怎么安静** | 「我已刷」推进水位线 ·「跳过这版」只压住这一版 ·「我知道了」只灭灯 |
| **要装什么** | ★ **零依赖** —— 不用 Home Assistant、不用 MQTT、不用服务器、不用装 App |

## 界面

| 网页面板（手机） | OLED 128×64（默认） | OLED 128×32（小屏） |
|:---:|:---:|:---:|
| <img src="docs/web-mobile.png" width="230" alt="网页面板"> | <img src="docs/oled-128x64.png" width="330" alt="OLED 128×64"> | <img src="docs/oled-128x32.png" width="330" alt="OLED 128×32"> |

---

## 为什么不用现成的？

像 [changedetection.io](https://github.com/dgtlmoon/changedetection.io) 那种工具功能强得多（能盯任意网页、有历史 diff、多种推送），
但它**需要一台常开的机器 + Docker**；而这件事本身只值「插上就不管」的 15 块钱。

所以 rom-watch 的取舍是：**只干一件事，但干得完全不用你操心** ——
上电就连网、10 分钟查一次、有新版本三处一起提醒、刷完按一下板载键就完事。

---

## 特性

| | |
|---|---|
| 🔀 **数据源可换** | OpenList 目录 / GitHub Release / 自定义 JSON —— 网页「监测设置」里选预设 + 填取值键即可 |
| ✅ **三级通知** | 大多数同类工具只有"提醒 / 不提醒"两态；这里把「**我已刷**」「**跳过这版**」「**我知道了（关灯）**」分开了 |
| 🕐 **水位线 + 首次发现** | 记住你确认过的版本；还能回答"**这个版本首次发现于什么时候**"（历史表只留 6 条，答不了这个） |
| 🖥 **显示自适应** | 0.96" OLED 128×64，也支持 128×32 小屏（**版面自动变两行**）；可选外挂 TJC 串口屏 |
| 🌐 **网页看板** | 整页装在 PROGMEM 里**流式发出**（**堆占用 ~0**）、5 秒轮询、`?kiosk=1` 看板模式、`?scale=` 字号档 |
| 📦 **启动黑匣子** | 启动次数 / 上次复位原因 / **崩在第几阶段**（存 RTC 内存，**异常重启也不丢**） |
| 🧰 **适配层** | 接线 / 屏幕尺寸 / 板型 / 数据源 / 标题 / 口令 —— **只改 `config.h` 一个文件**，主文件不动 |
| 📡 **无线 OTA** | 网页刷 / espota 推 / 拉取式刷，另带一块**保命底座固件**（刷坏了还能救回来） |

---

## 三分钟上手

**你需要**：WeMos D1 mini（或任意 ESP8266 开发板）＋ 0.96" I2C OLED（SSD1306/SSD1315，地址 0x3C）＋ 一根 USB 线。

```bash
# ① 拿代码
git clone https://github.com/cmyfqwq/rom-watch.git
cd rom-watch

# ② 配自己的值（★这一步必做 —— 仓库里只有样板，没有任何私有信息）
cd firmware/rom-watch
cp config.example.h config.h        # Windows: copy config.example.h config.h
#   然后编辑 config.h：填 WiFi，或把 PVT_WIFI_SET 留 0（开机走 AP 配网）

# ③ 装工具链（一次性）
arduino-cli core install esp8266:esp8266        # 本仓库实测 3.1.2
arduino-cli lib install "ESP8266 and ESP32 OLED driver for SSD1306 displays"

# ④ 编译 + 上传
arduino-cli compile --fqbn esp8266:esp8266:d1_mini firmware/rom-watch
arduino-cli upload  --fqbn esp8266:esp8266:d1_mini -p <你的串口> firmware/rom-watch
```

**接上电之后**：浏览器打开板子的 IP（串口日志里会打印），在「**监测设置**」里填服务器和目录 → 保存 → 点「**立即检查**」。
网盘（OpenList/alist）用预设 `0`；想盯某个 GitHub 仓库就选预设 `1`，填 `OWNER/REPO`。

> 网页里点「**怎么用（3 步）**」有一份写给非程序员看的说明 ✓

---

## 接线（默认值）

| 从 | 到 | 说明 |
|---|---|---|
| OLED `SDA` | ESP **D2** | I2C 数据 |
| OLED `SCL` | ESP **D1** | I2C 时钟 |
| OLED `VCC` / `GND` | 3V3 / GND | 0.96" 屏一般 3.3V 就够 |
| 串口屏 `TX` → `RX` | ESP **D5** / **D7** | ★**可选**，`USE_TJC=1` 时才用 |
| 串口屏 `VCC` / `GND` | **独立 5~12V** / 共地 | ⚠️ 屏工作电流 ~420mA，**别从板上取电** |
| 板载 LED / FLASH 键 | GPIO2 / GPIO0 | 板子自带的，不用额外接 |

> ⚠️⚠️ **串口屏绝对不要接 D6（GPIO12）** —— 那是 ESP8266 **开机时判断 flash 电压**的脚，
> 被串口占着可能直接**起不来**。默认已经避开它了，改接线时别改回去。

**详细接线、换板子/换屏的步骤、排错对号入座** → [docs/接线与适配.md](docs/接线与适配.md)

---

## 适配层：换硬件 / 换标题，只改 `config.h`

主文件里每个适配项都写成 `#ifndef`（**你定义了才覆盖**）⇒ 不写就走默认值，**主文件一个字不用动** ✓

| 宏 | 默认 | 什么时候改 |
|---|---|---|
| `PIN_OLED_SDA` / `PIN_OLED_SCL` | `D2` / `D1` | 换 I2C 引脚 |
| `OLED_ADDR` | `0x3C` | 少数模块是 `0x3D` |
| `OLED_SMALL` | `0` | 设 `1` ⇒ **128×32 小屏（0.91"）**，版面自动换成两行 |
| `PIN_TJC_RX` / `PIN_TJC_TX` | `D5` / `D7` | 换串口屏接线（★别用 D6） |
| `PIN_LED` / `PIN_BTN` | `LED_BUILTIN` / `D3` | 换板子 |
| `DFLT_TITLE` | `系统包监测` | **网页 / OLED / 串口屏三处共用的标题**（网页上也能随时改）★含新字要加进 `cn_extra.txt` 再重跑取模脚本 |
| `DFLT_HOST` / `DFLT_PATH` | 空 | 第一次开机的默认数据源（**留空是合法的**，固件会提示"还没配数据源"） |
| `OTA_PASSWORD` | 源码里的公开默认值 | ★**必须改** —— 写进 `config.h`，`tools/ota.py` 会**自动读它** |

板型 / 口令 / 串口也能用**环境变量**覆盖，不用改脚本：

```bash
set ROMWATCH_FQBN=esp8266:esp8266:nodemcuv2
set ROMWATCH_OTA_PASS=你自己的口令
set ROMWATCH_PORT=COM7
```

---

## 编译开关与资源

这块板子的 **IRAM 只有 64KB，全开就到 96%** ⇒ 做了编译开关：不用哪个就把哪个整块不编进来。

| 配置 | 编译开关 | RAM | IRAM | Flash |
|---|---|---|---|---|
| `full`（默认） | — | 49% | **96%** | 61% |
| `no_tjc` | `-DUSE_TJC=0` | 46% | 94% | 60% |
| `no_oled` | `-DUSE_OLED=0` | 49% | 96% | **50%** |
| `minimal` | 两个都 0 | 46% | 94% | 48% |
| `oled32` | `-DOLED_SMALL=1` | 49% | 96% | 61% |
| `portable` | **干净副本（不带 `config.h`）** | 49% | 96% | 61% |

```bash
python tools/build_matrix.py          # 一键跑上表全部配置，自己看实测数字
```

> `portable` 那一行是**「别人 clone 下来直接能编」的实测证据**：脚本把可公开的那几个文件复制到临时目录、
> **故意不带 `config.h`** 编一遍 ⇒ 编过了 ✓（不是嘴上说"能编"）

---

## 网页端点

<details>
<summary>点开看完整端点表（动作类都是 POST）</summary>

| 端点 | 方法 | 干什么 |
|---|---|---|
| `/` | GET | 网页面板 |
| `/rom` | GET | 状态 JSON（页面每 5 秒拉一次，也方便脚本读） |
| `/status` `/ping` `/log` | GET | 设备信息 / 存活探测 / 纯文本日志（含"启动黑匣子"） |
| `/check` | POST | 立即检查（秒回，抓包挪到主循环做） |
| `/ack` `/skip` | POST | 我已刷 / 跳过这版 |
| `/seen` | POST | 我知道了（关灯）；`?clear=1` 撤销 |
| `/led?on=1\|0\|2` | GET | 手动点灯 15 秒（分辨"灯坏了"还是"逻辑不该亮"） |
| `/cfg` | POST | 改数据源 / 路径 / 间隔 / 标题 / 水位线 |
| `/i2c` | GET | 扫 I2C 总线（看 OLED 在不在） |
| `/tjcwire` `/tjcsweep` `/tjccmd?c=` | GET | 串口屏线路体检 / 扫波特率 / 发任意指令 |
| `/update` `/reset` `/reconfig` | GET/POST | 网页刷固件 / 清凭据 / 重新配网 |

</details>

---

## 常见问题

| 现象 | 先看这里 |
|---|---|
| **网页打不开** | 板子还在不在网？看串口日志里的 IP；AP 模式下面板在 `192.168.4.1` |
| **OLED 不亮 / 花屏** | `/i2c` 扫得到 `0x3C` 吗？扫不到就是接线/地址问题（试试 `OLED_ADDR=0x3D`） |
| **屏上字缺笔画** | 字模是从源码里的汉字自动生成的 ⇒ 你在源码里加了新汉字，**要重跑取模脚本** |
| **改了标题、屏上是空方框** | 标题的字必须在**字模表**里 ⇒ 把新字写进 `firmware/rom-watch/cn_extra.txt`，再跑 `gen_cn_font.ps1`，然后重编上传 |
| **数据源一直失败** | `/rom` 的 `lastErr` 和「调试：最近响应片段」会直接告诉你原因 |
| **GitHub 预设抓不到** | 必须带 User-Agent（固件已带 ✓）；未登录**限流 60 次/小时/IP** ⇒ 间隔别小于 10 分钟 |
| **抓到的时间不对** | `/log` 看校时；没 NTP 时显示的是"开机计时" |
| **板子反复重启** | `/log` 看「上次跑到阶段」＋启动次数（启动黑匣子存在 RTC 内存里，异常重启也不丢） |
| **IRAM 超了编不过** | 跑 `python tools/build_matrix.py`，把用不到的开关关掉 |

---

## 附带工具（`tools/`）

| 工具 | 干什么 |
|---|---|
| `ota.py` | 一键 **编译 + 无线推送(OTA) + 校验**；也能 `find` 找板子、`serial` 看日志、`rescue` 串口兜底 |
| `build_matrix.py` | 六配置编译矩阵，打印 RAM/IRAM/Flash 对比 |
| `check_publish.py` | **发布前自检**：算出哪些文件会被提交 ＋ 扫私有信息（PASS/FAIL 退出码） |

> ★ **TJC / Nextion `.HMI` 工程格式的那套工具链已经单独拆成仓库**：
> [**cmyfqwq/tjc-hmi-toolkit**](https://github.com/cmyfqwq/tjc-hmi-toolkit)（[Gitee 镜像](https://gitee.com/cmyfqwq/tjc-hmi-toolkit)）
> —— 那边有完整的**格式逆向说明**（容器 / 目录 / 页面 TLV / 控件 / 事件代码）和一份诚实的"还没弄清的地方"清单。
>
> **★哪份是"正版"**：以 **tjc-hmi-toolkit 那份为准** ✓ —— 本仓库 `tools/` 里那份是**就地保留的副本**
> （因为作者的刷屏脚本引用了这个路径）；**两边内容目前一致，改了要一起改** ✓

---

## 目录结构

<details>
<summary>点开看</summary>

```
.
├── firmware/
│   ├── rom-watch/              # ★主固件
│   │   ├── rom-watch.ino       #   主程序
│   │   ├── web_ui.h            #   网页（整页装 PROGMEM 流式发出，服务端零动态渲染）
│   │   ├── cn_font.h           #   中文点阵（脚本自动生成，勿手改）
│   │   ├── cn_extra.txt        #   ★你标题里要用、而固件里没有的字写这儿
│   │   ├── config.example.h    #   ★配置样板 —— 复制成 config.h 再改
│   │   └── config.h            #   你的私有配置（**不进仓库**）
│   └── ota-base/               # 无线 OTA 底座固件（保命用，可单独刷）
├── tools/                      # ota.py / build_matrix.py / check_publish.py ＋ .HMI 工具副本
├── docs/                       # 文档与配图
├── 开关与配置说明_rom-watch.md  # 更细的开关 / EEPROM 布局 / 端点说明
└── .gitignore
```

</details>

---

## 安全提醒

1. **改掉默认 OTA 口令** —— 默认值写在源码里，**同网段的任何人都能刷你的固件**。
   在 `config.h` 里加一行 `#define OTA_PASSWORD "换成你自己的"` 就行（`tools/ota.py` 会自动读它 ✓）。
2. **`config.h` 是你的私有文件**（WiFi 密码、服务器地址）—— 已被 `.gitignore` 排除，**别手滑提交**。
3. **固件是明文 HTTP**（局域网内），别直接暴露到公网；要远程访问请走内网穿透 + 鉴权。
4. **编译产物 `.bin` 里会内嵌你编译时的凭据** ⇒ `build/` 目录同样不进仓库。

```bash
python tools/check_publish.py           # 发布前自检：PASS 退出码 0 / FAIL 退出码 1
python tools/check_publish.py --list    # 顺便列出"会被提交的文件"清单
```

> 它用**真 git 的 ignore 语义**算（临时建仓跑 `check-ignore`，跑完即删，不碰你的 `.git`）；
> 你自己的私有字面量（WiFi 名/密码/域名）写在 `tools/.publish-secrets.txt`（**本地文件，不进仓库**）。

---

## 许可证

**MIT** —— 见 [LICENSE](LICENSE)。可以自由使用、修改、分发（保留版权声明即可）。

## 相关文档

- [接线与适配](docs/接线与适配.md) —— 接线表、D6 禁令、换板子/换屏步骤、排错
- [开关与配置说明](开关与配置说明_rom-watch.md) —— 编译开关、EEPROM 布局（红线）、端点、命令速查
- [config.example.h](firmware/rom-watch/config.example.h) —— 所有可配置项的注释样板
