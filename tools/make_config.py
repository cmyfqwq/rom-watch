#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_config.py —— rom-watch 的**交互式配置向导**（新手向）
============================================================================
解决的问题：新手要手改 `firmware/rom-watch/config.h` —— 得懂 `#define`、
  得小心别把引号写错、还得知道哪些宏是干嘛的 ⇒ 劝退 ✗
本脚本把这件事变成「回答几个问题 + 回车」✓

它做的事：
  1. 用 `input()` 问几个问题（**每个都有默认值，直接回车就用默认** ✓）
  2. 生成 `firmware/<sketch>/config.h`，结构/注释风格与 `config.example.h` 一致 ✓
  3. 覆盖前先备份成 `config.h.bak-<日期>` ✓
  4. 结尾告诉你「下一步」怎么编译烧录 ✓

★只依赖 Python 标准库 ✓（不需要 pip install 任何东西）

用法：
  python tools\\make_config.py                      # 交互式，默认工程 rom-watch
  python tools\\make_config.py --sketch rom-watch   # 同上（显式）
  python tools\\make_config.py --out D:\\tmp\\cfg.h  # 写到别处（测试用，不动真实配置）
  python tools\\make_config.py --yes                # 全用默认值，不交互（自动化用 ✓）

★安全：脚本里**没有任何真实 WiFi 名 / 域名 / IP / 人名 / 个人路径** ——
  示例一律是 `alist.example.com`、`/public/roms`、`你的WiFi名` ✓
  `config.h` 是主人自己的私有文件，本脚本**只写不读** ✓
"""

import argparse
import datetime
import getpass
import os
import re
import secrets
import sys

# ---- Windows 控制台默认是 GBK ⇒ 先兜底改成 UTF-8，免得中文注释炸掉 ----
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass  # 老 Python / 已重定向的流：忽略即可，不致命


# ============================================================================
# 默认值（全部是**中性**的公开示例，不含任何私有信息 ✓）
# ============================================================================
DEFAULT_SKETCH = "rom-watch"
DEFAULT_TITLE = "系统包监测"          # 与固件里的字模默认值一致 ✓
DEFAULT_INTERVAL = 600                # 轮询秒（网页最小 60）
OLED_SIZES = {"1": (0, "128x64（0.96\" 大屏，默认）"),
              "2": (1, "128x32（0.91\" 小屏，版面自动变两行）")}

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)

# 危险字面量黑名单：这些字符会破坏 C 字符串字面量 ⇒ 直接拒绝并说清怎么办 ✓
BAD_CHARS = {'"': '英文双引号', '\\': '反斜杠', '\n': '换行', '\r': '回车', '\t': '制表符'}


class Abort(Exception):
    """用户主动放弃（Ctrl+C / 输入 EOF）。"""


def _say(msg=""):
    print(msg)


def _head(msg):
    _say()
    _say("=" * 68)
    _say(msg)
    _say("=" * 68)


def ask(prompt, default="", non_interactive=False, valid=None):
    """问一个问题。回车 = 用默认值 ✓ valid: 返回 True/None 表示合法，字符串 = 错误原因。"""
    if non_interactive:
        return default
    while True:
        tip = "{} [{}]: ".format(prompt, default) if default != "" else "{}: ".format(prompt)
        try:
            sys.stderr.write("[DBG-read] " + tip[:20] + "\n")
            ans = input(tip).strip()
            sys.stderr.write("[DBG-got] " + repr(ans) + "\n")
        except (EOFError, KeyboardInterrupt):
            raise Abort()
        if ans == "":
            ans = default
        if valid is not None:
            bad = valid(ans)
            if bad:
                _say("  ✗ " + bad)
                continue
        return ans


def ask_secret(prompt, non_interactive=False, default=""):
    """问密码：**输入时不回显** ✓（getpass）

    ★两个坑都堵住了（实测）：
      ① stdin 不是终端（被重定向 / 管道喂输入）时**不能**用 getpass ——
         Windows 上它会去读 CONIN$，没有控制台就**永久卡住**（本脚本 --yes 之外的
         自动化场景会死等）⇒ 这种情况退化成普通 input()，只是不回显不了 ✓
      ② getpass 抛别的异常（老环境 / 特殊终端）⇒ 同样退化成 input() ✓"""
    if non_interactive:
        return default
    if sys.stdin is not None and sys.stdin.isatty():
        try:
            return getpass.getpass(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            raise Abort()
        except Exception:
            pass  # 落到下面普通输入
    else:
        _say("  （输入源不是终端 ⇒ 密码没法隐藏显示，注意别被人看到屏幕）")
    try:
        return input(prompt).strip()
    except (EOFError, KeyboardInterrupt):
        raise Abort()


def check_literal(value, what):
    """拒绝会破坏 C 字符串的字符 ⇒ 返回错误原因或 None。"""
    for ch, name in BAD_CHARS.items():
        if ch in value:
            return "{} 里不能有{}（会把 config.h 里的引号弄坏）。换个写法，或改用别的字符。".format(what, name)
    if value.strip() != value:
        return "{} 首尾不能有空格。".format(what)
    return None


def gen_ota_password():
    """随机生成 OTA 口令（默认值）—— 只用字母数字，保证写进 C 字符串一定安全 ✓"""
    alphabet = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # 去掉易混的 l/I/O/0/1
    body = "".join(secrets.choice(alphabet) for _ in range(10))
    return "rw-ota-" + body


# ============================================================================
# 交互问答
# ============================================================================
def collect(args):
    cfg = {
        "ssid": "",
        "pass": "",
        "host": "",
        "path": "",
        "preset_note": "",
        "title": DEFAULT_TITLE,
        "oled_small": 0,
        "ota_pass": gen_ota_password(),
        "interval": DEFAULT_INTERVAL,
        "src_kind": "none",
        "json_key": "",
        "time_key": "",
    }
    ni = args.yes

    if ni:
        _head("全默认模式（--yes）：不问你，直接按默认值生成")
    else:
        _head("rom-watch 配置向导 —— 每个问题直接按回车 = 用方括号里的默认值 ✓")
        _say("（Ctrl+C 可以随时放弃，不会写任何文件）")

    # ---------- ① WiFi ----------
    _head("第 1 步 / 6：WiFi（直接回车 = 走 AP 配网，更安全 ✓）")
    _say("说明：写死 WiFi 的好处是插上就自己连；不写的话，第一次开机板子会自己开一个")
    _say("      热点（名字以 ROMWatch- 开头），你用手机连上去填 WiFi —— 这样 config.h 里")
    _say("      就没有密码，将来要分享/开源也放心 ✓")
    cfg["ssid"] = ask("你的 WiFi 名（2.4G 的；留空 = 不写死，走 AP 配网）",
                      "", ni, lambda v: check_literal(v, "WiFi 名") if v else None)
    sys.stderr.write("[DBG0] after ssid ask, ssid={!r}\n".format(cfg["ssid"]))
    if cfg["ssid"]:
        sys.stderr.write("[DBG1] entering password block\n")
        cfg["pass"] = ask_secret("WiFi 密码（输入时不显示，放心输；直接回车 = 密码为空）", ni)
        sys.stderr.write("[DBG2] password block done\n")
        bad = check_literal(cfg["pass"], "WiFi 密码")
        if bad:
            _say("  ✗ " + bad)
            raise Abort()
        _say("  ✓ 已记下（不会显示出来）")

    # ---------- ② 数据源 ----------
    _head("第 2 步 / 6：盯什么？（直接回车 = 先不配，开机后到网页上配 ✓）")
    _say("  1 = OpenList / alist 的某个目录（网盘里有没有新文件）")
    _say("  2 = GitHub 仓库的 Release（有没有发新版本）")
    _say("  3 = 任意 JSON 接口（自己填字段名）")
    _say("  回车 = 先不配 —— 开机后浏览器打开板子 IP，在「监测设置」里填 ✓")
    choice = ask("选一个（1/2/3，回车 = 先不配）", "", ni,
                 lambda v: None if v in ("", "1", "2", "3") else "只能填 1、2、3，或者直接回车。")
    sys.stderr.write("[DBG] ssid={!r} choice={!r}\n".format(cfg["ssid"], choice))

    if choice == "1":
        cfg["src_kind"] = "openlist"
        cfg["host"] = ask("网盘服务器域名（例：alist.example.com，别带 http:// 和结尾的 /）",
                          "alist.example.com", ni,
                          lambda v: check_literal(v, "服务器域名"))
        cfg["path"] = ask("要盯的目录（例：/public/roms，用 / 开头）",
                          "/public/roms", ni,
                          lambda v: check_literal(v, "目录"))
        if not cfg["path"].startswith("/"):
            _say("  ✗ 目录要以 / 开头（例：/public/roms）。")
            raise Abort()
        cfg["json_key"], cfg["time_key"] = "name", "created"
        cfg["preset_note"] = "OpenList 预设：取值键 name / 时间键 created"
    elif choice == "2":
        cfg["src_kind"] = "github"
        cfg["host"] = "api.github.com"
        repo = ask("GitHub 仓库（写成 主人/仓库名，例：octocat/Hello-World）",
                   "owner/repo", ni,
                   lambda v: None if re.match(r"^[^\s/]+/[^\s/]+$", v) else
                   "要写成 主人/仓库名（中间一个斜杠），例：octocat/Hello-World。")
        cfg["path"] = "/repos/{}/releases/latest".format(repo)
        cfg["json_key"], cfg["time_key"] = "tag_name", "published_at"
        cfg["preset_note"] = "GitHub 预设：取值键 tag_name / 时间键 published_at"
        _say("  ★提醒：GitHub 不登录时每小时只允许查 60 次 ⇒ 轮询间隔别小于 600 秒。")
    elif choice == "3":
        cfg["src_kind"] = "custom"
        cfg["host"] = ask("接口域名（例：api.example.com，别带 http://）", "api.example.com", ni,
                          lambda v: check_literal(v, "接口域名"))
        cfg["path"] = ask("请求路径（例：/version.json，用 / 开头）", "/version.json", ni,
                          lambda v: check_literal(v, "请求路径"))
        cfg["json_key"] = ask("JSON 里哪个字段是版本号？", "name", ni,
                              lambda v: check_literal(v, "字段名"))
        cfg["time_key"] = ask("JSON 里哪个字段是时间（用来比新旧）？", "created", ni,
                              lambda v: check_literal(v, "字段名"))
        cfg["preset_note"] = "自定义 JSON 预设"
    else:
        cfg["src_kind"] = "none"
        _say("  ✓ 先不配 —— 开机后板子会提示「还没配数据源：网页「监测设置」里填」✓")

    # ---------- ③ 标题 ----------
    _head("第 3 步 / 6：标题（网页 / OLED / 串口屏三处共用）")
    cfg["title"] = ask("屏幕上显示什么标题", DEFAULT_TITLE, ni,
                       lambda v: check_literal(v, "标题") or
                       (None if v else "标题不能是空的（直接回车就用默认的「{}」）。".format(DEFAULT_TITLE)))
    _say("  ★提醒：标题里的每个字都必须在字模表里，否则屏上画成**空方框** ✗")
    _say("    换了标题、里面有新字 ⇒ 把那些字写进 firmware\\{}\\cn_extra.txt，".format(args.sketch))
    _say("    再重跑取模脚本 gen_cn_font.ps1，然后重新编译上传 ✓")

    # ---------- ④ 屏幕尺寸 ----------
    _head("第 4 步 / 6：OLED 尺寸")
    _say("  1 = 128x64（0.96\" 大屏 —— 默认，最常见）")
    _say("  2 = 128x32（0.91\" 小屏 —— 版面会自动变成两行）")
    size = ask("选一个（1/2）", "1", ni,
               lambda v: None if v in OLED_SIZES else "只能填 1 或 2。")
    cfg["oled_small"], cfg["oled_desc"] = OLED_SIZES[size]
    _say("  ★这个改完必须**重新编译上传**才生效（网页上改不了）✓")

    # ---------- ⑤ OTA 口令 ----------
    _head("第 5 步 / 6：OTA 口令（无线刷固件用的密码）")
    _say("为什么必须改：固件源码里那个默认口令是**公开的** ⇒ 同网段的人都能刷你的板子 ✗")
    _say("下面这个随机口令是我替你生成的，直接回车就用它 ✓")
    ota = ask("OTA 口令（回车 = 用随机生成的这个）", cfg["ota_pass"], ni,
              lambda v: check_literal(v, "OTA 口令") or
              (None if v else "口令不能是空的。"))
    cfg["ota_pass"] = ota

    # ---------- ⑥ 轮询间隔 ----------
    _head("第 6 步 / 6：多久查一次")
    _say("单位是秒。默认 600 秒 = 10 分钟（够用，也不容易被人家的服务器限流 ✓）")

    def valid_interval(v):
        if not v.isdigit():
            return "要填一个整数秒数，例：600。"
        if int(v) < 60:
            return "固件要求最小 60 秒（网页上也是这个下限）。"
        return None

    cfg["interval"] = int(ask("轮询间隔（秒）", str(DEFAULT_INTERVAL), ni, valid_interval))
    return cfg


# ============================================================================
# 生成 config.h（结构 / 注释风格照 config.example.h ✓）
# ============================================================================
def c_literal(value):
    """把值变成 C 字符串字面量（引号内已保证没有 " 和 \\ ✓）"""
    return '"{}"'.format(value)


def render_config(cfg, sketch):
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    L = []
    A = L.append

    A("/*")
    A(" * config.h —— {} **本机私有配置**（编译开关 + 你的默认值）".format(sketch))
    A(" * " + "=" * 74)
    A(" * ★本文件由 `tools\\make_config.py`（交互式配置向导）于 {} 生成 ✓".format(now))
    A(" *   想改哪一项，重跑一次向导最省事；当然手改也完全可以 ✓")
    A(" *")
    A(" * ★为什么这个文件单独存在：主文件 rom-watch.ino 用 `#if __has_include(\"config.h\")`")
    A(" *   探测它 ⇒ ① **没有 config.h 也能编译**（走中性默认值 + 开机 AP 配网 ✓）")
    A(" *   ② 真实凭据只在这里 ⇒ 它**永远不进仓库**（.gitignore 已排掉 ✓）")
    A(" *   ③ 所以仓库里只放一份\"空壳样板\" config.example.h ✓")
    A(" * " + "=" * 74)
    A(" */")
    A("#pragma once")
    A("")
    A("/* ===== ① 编译期功能开关 =====================================================")
    A(" * 写成 #ifndef ⇒ 也可以**从命令行覆盖**（不用改本文件）：")
    A(" *     arduino-cli compile ... --build-property \"compiler.cpp.extra_flags=-DUSE_TJC=0\"")
    A(" *   ⇒ 多配置编译矩阵就靠这个（tools\\build_matrix.py ✓）")
    A(" * 用途：IRAM 只有 64KB，全开是 96% ⇒ 不用哪个就把哪个关掉 ✓ */")
    A("#ifndef USE_TJC")
    A("#define USE_TJC    1        // 串口屏（SoftwareSerial + 坐标驱动 + 状态推送），最吃 IRAM")
    A("#endif")
    A("#ifndef USE_OLED")
    A("#define USE_OLED   1        // 0.96\" OLED（含中文字模，吃 flash 不吃 IRAM）")
    A("#endif")
    A("")
    A("/* ===== ② 你的私有默认值（首次烧录 / EEPROM 被清时用它）=====================")
    A(" * ★PVT_WIFI_SET 是个**数字**开关，必须这样写 —— 预处理器的 #if **不能对字符串字面量取下标**")
    A(" *   （`#if PVT_WIFI_SSID[0]` 会报 `token \"\"x\"\" is not valid in preprocessor expressions` ✗，实测）")
    A(" *   1 = 开机强制写入下面这套 WiFi 凭据（\"写死一劳永逸\"）")
    A(" *   0 = 不写，开机走 AP 配网（★**要分享/开源就用 0** ✓）*/")
    A("#define PVT_WIFI_SET   {}".format(1 if cfg["ssid"] else 0))
    if cfg["ssid"]:
        A("#define PVT_WIFI_SSID  {}                 // 2.4G".format(c_literal(cfg["ssid"])))
        A("#define PVT_WIFI_PASS  {}".format(c_literal(cfg["pass"])))
    else:
        A("#define PVT_WIFI_SSID  \"你的WiFi名\"                 // 2.4G")
        A("#define PVT_WIFI_PASS  \"你的WiFi密码\"")
        A("/* ★上面两行是**占位符**，没生效（PVT_WIFI_SET=0）—— 想写死就重跑向导填 WiFi，")
        A(" *   或把 PVT_WIFI_SET 改成 1 并填上真值 ✓ */")
    A("")
    A("/* 默认数据源（网页「监测设置」里随时能改，这里只是\"第一次开机\"的初值）")
    if cfg["src_kind"] == "none":
        A(" * ★现在是**空的** = 还没配：固件会直接给一句人话（\"还没配数据源：网页「监测设置」里填\" ✓），")
        A(" *   不会拿着空主机名去握手 ✓ —— 所以**空着是完全合法的默认值** ✓ */")
    else:
        A(" * ★本次向导选的源：{}".format(cfg["preset_note"]))
        if cfg["src_kind"] == "github":
            A(" *   ★GitHub 不登录时限流 60 次/小时/IP ⇒ 轮询别小于 600 秒（固件已自动带 User-Agent ✓）")
        if cfg["src_kind"] == "custom":
            A(" *   ★取值键 / 时间键**不在这个文件里**（它们存在板子的 EEPROM 里）⇒ 见下方 ③ 的说明 ✗✓")
        A(" *   例：alist.example.com / /public/roms ✓ */")
    A("#define DFLT_HOST      {}                          // 例：alist.example.com".format(
        c_literal(cfg["host"])))
    A("#define DFLT_PATH      {}                          // 例：/public/roms".format(
        c_literal(cfg["path"])))
    A("#define DFLT_INTERVAL  {}                         // 轮询秒（网页最小 60）".format(
        cfg["interval"]))
    A("")
    A("/* ★标题（网页 / OLED / 串口屏 **三处共用**）")
    A(" *   网页「监测设置 → 标题」里随时能改 ✓，这里只是\"第一次开机\"的初值。")
    A(" *   ⚠️ **标题里的每个字都必须在字模表里**，否则屏上画成**空方框** ✗")
    A(" *     ⇒ 换了标题、里面有新字：把它们写进同目录的 `cn_extra.txt`，")
    A(" *       再重跑 `gen_cn_font.ps1` 重新生成字模 ✓✓")
    A(" *   例： #define DFLT_TITLE  \"我的网盘更新\"   */")
    A("#define DFLT_TITLE     {}".format(c_literal(cfg["title"])))
    A("")
    A("/* ★OTA 口令（无线刷固件 / 网页 /update 都用它）—— 本次由向导生成")
    A(" *   为什么必须改：固件源码里那个默认口令是**公开的** ⇒ 同网段的人能直接刷你的板子 ✗")
    A(" *   真实口令只写在这个文件里（它**不进仓库** ✓）")
    A(" *   ★`tools\\ota.py` 会**自动读这一行** ⇒ 改了这里就够，不用再去改脚本 ✓✓")
    A(" *   环境变量 ROMWATCH_OTA_PASS 优先级更高，可临时顶替 ✓ */")
    A("#define OTA_PASSWORD   {}".format(c_literal(cfg["ota_pass"])))
    A("")
    A("/* ===== ③ 硬件适配（接线 / 显示屏 / 按键）—— 全是可选的 ======================")
    A(" * 一条都不写就走主文件里的默认值（WeMos D1 mini 标准接线 ✓）")
    A(" * ⇒ 换板子 / 换屏 / 换接线时，**只在这个文件里加几行**，主文件一个字都不用动 ✓✓")
    A(" *")
    A(" *   #define PIN_OLED_SDA  D2           // I2C 数据（默认 D2）")
    A(" *   #define PIN_OLED_SCL  D1           // I2C 时钟（默认 D1）")
    A(" *   #define OLED_ADDR     0x3C         // 少数模块是 0x3D")
    A(" *   #define OLED_SMALL    1            // 128x32 的小屏（0.91\"）—— 版面自动变两行 ✓")
    A(" *   #define PIN_TJC_RX    D5           // 串口屏 TX → 板 D5")
    A(" *   #define PIN_TJC_TX    D7           // 串口屏 RX → 板 D7（★别用 D6：开机判 flash 电压）")
    A(" *   #define PIN_LED       LED_BUILTIN  // 板载 LED（低电平点亮）")
    A(" *   #define PIN_BTN       D3           // 板载 FLASH 键（按下为低）")
    A(" *   #define OTA_PASSWORD  \"换成你自己的\"  // ★分享/开源前**务必**改掉默认口令 ✓")
    A(" *                                        //   （`tools/ota.py` 会**自动读 `config.h` 里这一行** ⇒")
    A(" *                                        //    口令只有一个真值源，不用再去改脚本 ✓；")
    A(" *                                        //    环境变量 `ROMWATCH_OTA_PASS` 优先级更高，可临时顶替 ✓）")
    if cfg["oled_small"]:
        A(" *")
        A(" * ★本次向导按你选的\"小屏\"打开了这一行（128x32 ⇒ 版面自动两行）✓ */")
        A("#define OLED_SMALL    1            // 128x32 小屏（0.91\"）—— 改完要重新编译上传 ✓")
    else:
        A(" * ============================================================================ */")
    A("")
    if cfg["src_kind"] != "none":
        A("/* ===== ④ 数据源细节（★只有网页能设，不在这个文件里）========================")
        A(" * 固件把\"预设 / 取值键 / 时间键\"存在**板子的 EEPROM** 里（开机默认走 OpenList 预设），")
        A(" *   config.h 里没有对应的宏 —— 写了也不生效 ✗（这是固件的设计，不是向导漏了 ✓）")
        A(" *   ⇒ 烧录完成后，浏览器打开板子 IP →「监测设置」：")
        if cfg["src_kind"] == "openlist":
            A(" *       预设选「OpenList 目录」；取值键 name、时间键 created（默认就是这组 ✓）")
        elif cfg["src_kind"] == "github":
            A(" *       预设选「GitHub Release」；取值键 tag_name、时间键 published_at（默认就是这组 ✓）")
        else:
            A(" *       预设选「自定义 JSON」；取值键填 {}、时间键填 {}".format(
                cfg["json_key"], cfg["time_key"]))
        A(" *       再把服务器 / 路径核对成上面 DFLT_HOST / DFLT_PATH 的值 ⇒ 保存 → 点「立即检查」✓")
        A(" * ============================================================================ */")
        A("")
    A("/* ★还想加别的适配项？看 `config.example.h` 那一节，把注释去掉改成你的值即可 ✓ */")
    return "\n".join(L) + "\n"


# ============================================================================
# 落盘（先备份 ✓）
# ============================================================================
def write_out(path, text):
    out_dir = os.path.dirname(os.path.abspath(path))
    if out_dir and not os.path.isdir(out_dir):
        try:
            os.makedirs(out_dir, exist_ok=True)
        except OSError as e:
            _say("✗ 建不了目录：{}".format(out_dir))
            _say("  原因：{}".format(e))
            _say("  怎么办：确认这个路径的父目录存在、而且你有写权限；或换个 --out 路径。")
            return None
    backup = None
    if os.path.exists(path):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        backup = "{}.bak-{}".format(path, stamp)
        try:
            with open(path, "rb") as src, open(backup, "wb") as dst:
                dst.write(src.read())
        except OSError as e:
            _say("✗ 备份失败：{}".format(backup))
            _say("  原因：{}".format(e))
            _say("  怎么办：文件可能被编辑器占用（关掉再试），或没有写权限（换个目录）。")
            return None
        _say("✓ 已备份原文件 → {}".format(os.path.relpath(backup, REPO_ROOT)))
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        _say("✗ 写不了文件：{}".format(path))
        _say("  原因：{}".format(e))
        _say("  怎么办：① 文件是不是被 Arduino IDE / 编辑器占着？关掉重试；")
        _say("          ② 目录只读 / 无权限？换个 --out 路径试。")
        return None
    return backup


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="make_config.py",
        description="rom-watch 配置向导 —— 回答几个问题，自动生成 firmware/<sketch>/config.h ✓",
        epilog="例：python tools\\make_config.py --sketch rom-watch --yes")
    ap.add_argument("--sketch", default=DEFAULT_SKETCH,
                    help="固件工程名（对应 firmware\\<名字>\\），默认 {}".format(DEFAULT_SKETCH))
    ap.add_argument("--out", default=None,
                    help="写到哪个文件，默认 firmware\\<sketch>\\config.h")
    ap.add_argument("--yes", action="store_true",
                    help="全用默认值，不问你（给自动化用 ✓）")
    ap.add_argument("--force", action="store_true",
                    help="允许覆盖已存在的目标文件（默认会先问你一句，防手滑）")
    args = ap.parse_args(argv)

    out_path = args.out or os.path.join(REPO_ROOT, "firmware", args.sketch, "config.h")

    sketch_dir = os.path.join(REPO_ROOT, "firmware", args.sketch)
    if not os.path.isdir(sketch_dir):
        _say("✗ 找不到固件工程目录：{}".format(sketch_dir))
        _say("  怎么办：① 确认你在仓库根目录下跑这个脚本；")
        _say("          ② 或者用 --sketch <名字> 指定正确的工程名；")
        _say("          ③ 当前 firmware\\ 下有的是：")
        fw = os.path.join(REPO_ROOT, "firmware")
        try:
            for name in sorted(os.listdir(fw)):
                if os.path.isdir(os.path.join(fw, name)):
                    _say("             - {}".format(name))
        except OSError:
            _say("             （读不到 firmware\\ 目录）")
        return 2

    try:
        cfg = collect(args)
    except Abort:
        _say()
        _say("已放弃 —— 没有写任何文件 ✓（想重来就再跑一次）")
        return 130

    # 手滑保护：目标已存在且不是 --yes/--force ⇒ 问一句
    if os.path.exists(out_path) and not (args.yes or args.force):
        _say()
        _say("⚠️ {} 已经存在了。".format(os.path.relpath(out_path, REPO_ROOT)))
        _say("   （它可能是你自己配好的真配置 —— 覆盖前我会先备份成 .bak-<日期> ✓）")
        ans = ask("确定要覆盖它吗？（输入 y 覆盖，直接回车 = 放弃）", "", False,
                  lambda v: None if v.lower() in ("", "y", "yes", "n", "no") else "填 y 或直接回车。")
        if ans.lower() not in ("y", "yes"):
            _say("好，什么都没动 ✓")
            return 0

    text = render_config(cfg, args.sketch)
    backup = write_out(out_path, text)
    if backup is None and not os.path.exists(out_path):
        return 1

    # ---------------- 汇报 ----------------
    _head("✓ 配置写好了")
    _say("文件：{}".format(os.path.relpath(out_path, REPO_ROOT)))
    _say("  · WiFi        ：{}".format(
        "已写死（PVT_WIFI_SET=1）" if cfg["ssid"] else "没写死（PVT_WIFI_SET=0）⇒ 开机走 AP 配网"))
    _say("  · 数据源      ：{}".format({
        "openlist": "OpenList 目录  {} {}".format(cfg["host"], cfg["path"]),
        "github": "GitHub Release  {}".format(cfg["path"]),
        "custom": "自定义 JSON  {}{}".format(cfg["host"], cfg["path"]),
        "none": "先不配（开机后到网页上配）",
    }[cfg["src_kind"]]))
    _say("  · 标题        ：{}".format(cfg["title"]))
    _say("  · OLED        ：{}".format(cfg["oled_desc"]))
    _say("  · 轮询间隔    ：{} 秒".format(cfg["interval"]))
    _say("  · OTA 口令    ：{}".format(cfg["ota_pass"]))
    _say("    ★★ **这就是你要改掉的那个默认口令** —— 记下来，`tools\\ota.py` 会自动读它 ✓")

    _head("下一步")
    _say("  1) 编译 + 上传（推荐，一条命令全干完）：")
    _say("       pwsh -File tools\\flash.ps1")
    _say("     若你的仓库里没有 tools\\flash.ps1（老版本 / 精简版），就用：")
    _say("       python tools\\ota.py push --sketch {}".format(args.sketch))
    _say("")
    _say("  2) 接上电之后：浏览器打开板子 IP（串口日志里会打印），")
    _say("     在「监测设置」里核对数据源 → 保存 → 点「立即检查」✓")
    if cfg["src_kind"] == "none":
        _say("     （你这次没配数据源，所以**必须**在这一步填，否则它没法工作）")
    _say("")
    _say("  3) 改了标题里有新字 ⇒ 先把字写进 firmware\\{}\\cn_extra.txt，".format(args.sketch))
    _say("     再跑 gen_cn_font.ps1 重新生成字模，然后重新编译上传 ✓")
    _say("")
    _say("  ⚠️ config.h 是你的私有文件（WiFi 密码 / 服务器地址）—— 别手滑提交进 git ✓")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        _say()
        _say("已中断 —— 没有写任何文件 ✓")
        sys.exit(130)
