#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ota.py —— ESP8266 OTA 底座「一键操作台」

用法（在 serial-screen 目录下）：
    python tools/ota.py find              找板子（先查历史IP → 再扫网段 → 再听广播）
    python tools/ota.py build             编译（自动写入 FW_BUILD 时间戳）
    python tools/ota.py push              编译 + OTA 推送 + 校验固件真的换了  ← 日常就用这条
    python tools/ota.py pull              板子从本机 HTTP 拉固件（第三种通道）
    python tools/ota.py status            打印板子 /status
    python tools/ota.py ports             列出系统串口（自动探测）
    python tools/ota.py serial [--sec 10] 看串口日志
    python tools/ota.py rescue            兜底：串口烧录（FT232，板子刷砖也能救）
    python tools/ota.py portal            配网指引（板子连不上时）

依赖：系统 python 的 pyserial（已装 3.5）、arduino-cli、ESP8266 core（版本任意，自动探测）
      —— 跨平台：Windows / Linux / macOS 都能跑，路径不写死（见下面「工具链自动探测」一节）
"""
from __future__ import annotations

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:  # Windows 中文控制台
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ----------------------------------------------------------------- 路径与常量
# 跨平台：只用 pathlib，绝不用 "\\" 拼路径、绝不写死 .exe / 版本号 / 用户目录 ✓
ROOT   = Path(__file__).resolve().parent.parent           # serial-screen/
FWROOT = ROOT / "firmware"
STATE  = FWROOT / "ota-state.json"

# 工具链自动探测（延迟解析 + 缓存）：
#   ①环境变量 ARDUINO_CLI / ARDUINO_CLI_CONFIG ②仓库内 tools/ 自带 ③PATH
ARDUINO_CLI_URL = "https://arduino.github.io/arduino-cli/latest/installation/"

_cli_cache = None


def cli_path():
    """找 arduino-cli —— 顺序：环境变量 → 仓库自带 → PATH → 报错（给官网链接，不静默失败 ✓）

    注意：**不写死 `.exe`** —— Windows 叫 arduino-cli.exe，Linux/macOS 叫 arduino-cli，
    用 shutil.which 让平台自己挑（which 在 Windows 上也会补 .exe/.cmd 后缀）✓"""
    global _cli_cache
    if _cli_cache:
        return _cli_cache

    cands = []
    env = os.environ.get("ARDUINO_CLI")
    if env:
        cands.append(Path(env).expanduser())
    bundled = ROOT / "tools" / "arduino-cli"
    cands.append(bundled / "arduino-cli")          # 无后缀：Linux/macOS 的原生可执行
    cands.append(bundled / "arduino-cli.exe")      # 无后缀那个在 Windows 上不存在，回退即可
    for c in cands:
        if c.is_file():
            _cli_cache = c
            return c

    found = shutil.which("arduino-cli")            # PATH（含 arduino-cli.exe）
    if found:
        _cli_cache = Path(found)
        return found

    die("找不到 arduino-cli。三种装法任选：\n"
        f"  ①装到 PATH：看官网 {ARDUINO_CLI_URL}\n"
        f"  ②放进仓库：{bundled / 'arduino-cli'}（Windows 用 arduino-cli.exe）\n"
        "  ③指定环境变量：export ARDUINO_CLI=/path/to/arduino-cli")


def cli_cfg_args():
    """本机专用的 arduino-cli.yaml 只在存在时才传 --config-file；
    不存在就什么都不传 ⇒ 让 arduino-cli 用自己默认的配置（别人机器上没这个文件 ✓）"""
    for p in (os.environ.get("ARDUINO_CLI_CONFIG"),
              ROOT / "tools" / "arduino15" / "arduino-cli.yaml"):
        if p and Path(p).expanduser().is_file():
            return ["--config-file", str(Path(p).expanduser())]
    return []


_espota_cache = None


def espota_path():
    """找 espota.py —— 在 esp8266 core 目录下 glob 出**所有版本**，取版本号最高的那个 ✓
    （以前写死 3.1.2：别人装 3.0.2 / 3.1.3 就直接找不到 ✗）"""
    global _espota_cache
    if _espota_cache:
        return _espota_cache

    pat = (ROOT / "tools" / "arduino15" / "packages" / "esp8266"
           / "hardware" / "esp8266" / "*" / "tools" / "espota.py")
    hits = [Path(p) for p in glob.glob(str(pat)) if Path(p).is_file()]

    def ver_key(p):
        """3.1.10 > 3.1.2（纯字符串比较会排反 ⇒ 按数字元组比）"""
        v = p.parents[1].name
        parts = tuple(int(n) for n in re.findall(r"\d+", v))
        return (parts, v)

    if hits:
        _espota_cache = max(hits, key=ver_key)
        return _espota_cache

    die("找不到 espota.py（ESP8266 core 还没装）。先跑：\n"
        "  arduino-cli core update-index\n"
        "  arduino-cli core install esp8266:esp8266\n"
        f"（默认会装到 {ROOT / 'tools' / 'arduino15' / 'packages' / 'esp8266'} 下）")


SKETCH_NAME = None            # 由 --sketch 决定；没给就读 state，再退到 ota-base

def sk_name():
    return SKETCH_NAME or load_state().get("sketch") or "ota-base"
def sk_dir():   return FWROOT / sk_name()
def sk_build(): return FWROOT / "build" / sk_name()        # 每个工程独立产物目录，免混淆
def sk_bin():   return sk_build() / (sk_name() + ".ino.bin")
def sk_stamp(): return sk_dir() / "fw_build.h"

# ★2026-09-30 适配：板型和 OTA 口令都可用环境变量覆盖（换 ESP8266 开发板/改口令时不用改脚本 ✓）
#   set ROMWATCH_FQBN=esp8266:esp8266:nodemcuv2
#   set ROMWATCH_OTA_PASS=临时用别的口令
FQBN      = os.environ.get("ROMWATCH_FQBN", "esp8266:esp8266:d1_mini")


def _pass_from_config():
    """从 `<工程>/config.h` 里读 `OTA_PASSWORD` —— **口令的唯一真值源** ✓
    起因（2026-09-30 主人：「OTA 口令不轮换？？？」）：口令原来散在**固件宏**和**本脚本默认值**
    两处 ⇒ 改一处忘一处就是 OTA 失败 ✗。现在脚本直接读你 config.h 里那一行 ⇒ 只改一处 ✓"""
    f = sk_dir() / "config.h"
    if not f.exists():
        return None
    try:
        txt = f.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    m = re.search(r'^\s*#define\s+OTA_PASSWORD\s+"([^"]*)"', txt, re.M)
    return m.group(1) if m else None


def ota_pass():
    """优先级：环境变量 > config.h > 固件里的公开默认值 ✓"""
    return (os.environ.get("ROMWATCH_OTA_PASS")
            or _pass_from_config()
            or "meow-ota-8266")
OTA_PORT  = 8266              # ArduinoOTA 的端口（espota 用）
UDP_PORT  = 4210              # 板子 UDP 广播播报的端口（必须 != OTA_PORT，否则会抢 OTA 握手包）
HTTP_PORT = 8080
BAUD      = 115200
# 串口默认「自动探测」：环境变量 ROMWATCH_PORT 优先（跨平台手指定端口用这个 ✓）
#   Linux/macOS: export ROMWATCH_PORT=/dev/ttyUSB0     Windows: set ROMWATCH_PORT=COM5
PREFER_PORT = os.environ.get("ROMWATCH_PORT")           # 可选；没给就自动探测
USB_HINT = ("FTDI", "FT232", "CH340", "CP210", "USB Serial", "USB-SERIAL")

# ----------------------------------------------------------------- 小工具
def say(msg=""):
    print(msg, flush=True)


def die(msg, code=1):
    say(f"❌ {msg}")
    sys.exit(code)


def run(cmd, **kw):
    say("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw)


def http_get(url, timeout=1.5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except Exception:
        return None


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def load_state():
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_state(**kw):
    st = load_state()
    st.update(kw)
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(st, ensure_ascii=False, indent=2), encoding="utf-8")


def list_ports():
    """列出系统串口（跨平台）：
       Windows  → pyserial 的 list_ports（就是 GetPortNames 的等价物 ✓）
       Linux/macOS → glob('/dev/ttyUSB*' + '/dev/ttyACM*')，pyserial 在就再补充说明文字 ✓"""
    out = []
    try:
        from serial.tools import list_ports as lp
        for p in lp.comports():
            out.append({"device": p.device,
                        "desc": (p.description or p.manufacturer or "").strip()})
    except Exception:
        pass
    if not out and os.name != "nt":
        for g in ("/dev/ttyUSB*", "/dev/ttyACM*", "/dev/cu.usbserial*", "/dev/cu.usbmodem*"):
            for d in sorted(glob.glob(g)):
                out.append({"device": d, "desc": ""})
    return out


def find_port(required=False):
    """串口优先级：①环境变量 ROMWATCH_PORT ②名字/描述像 FTDI/CH340/USB 的口 ③（仅 Windows）COM5
    找不到时：required=True 报错并提示怎么指定，否则返回 None（让上层优雅降级 ✓）"""
    ports = list_ports()

    if PREFER_PORT:
        for p in ports:
            if p["device"].upper() == PREFER_PORT.upper():
                return p["device"]
        return PREFER_PORT                      # 环境变量指了就用它（哪怕现在没插上）

    for p in ports:
        if any(k in p["desc"] for k in USB_HINT):
            return p["device"]

    if os.name == "nt":
        for p in ports:                         # 老行为兜底：Windows 上默认 COM5 ✓
            if p["device"].upper() == "COM5":
                return p["device"]
        return "COM5"

    if len(ports) == 1:
        return ports[0]["device"]
    if len(ports) > 1:
        say("🔌 探到多个串口，用 --port 或环境变量 ROMWATCH_PORT 指定要哪个：")
        for p in ports:
            say(f"   {p['device']}   {p['desc']}")
        return None

    if required:
        die("没探到任何串口。插好 USB 转串口线再试，或显式指定：\n"
            "   Linux/macOS: --port /dev/ttyUSB0   或   export ROMWATCH_PORT=/dev/ttyUSB0\n"
            "   Windows:     --port COM5           或   set ROMWATCH_PORT=COM5")
    return None


# ----------------------------------------------------------------- find
def board_alive(ip, timeout=0.8):
    """只认自家板子：/ping 必须返回含 ok/fw/ver 的 JSON（否则路由器之类的会假阳性）"""
    txt = http_get(f"http://{ip}/ping", timeout=timeout)
    if not txt:
        return None
    try:
        data = json.loads(txt)
    except Exception:
        return None
    if not (isinstance(data, dict) and data.get("ok") is True and "fw" in data):
        return None
    return data


def scan_subnet(timeout=0.5, workers=64):
    my = local_ip()
    base = ".".join(my.split(".")[:3])
    say(f"🔍 扫网段 {base}.1-254 …（本机 {my}）")
    found = []

    def probe(i):
        ip = f"{base}.{i}"
        info = board_alive(ip, timeout)
        return (ip, info) if info else None

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        for res in ex.map(probe, range(1, 255)):
            if res:
                found.append(res)
    return found


def listen_udp(seconds=35):
    say(f"👂 监听 UDP 广播 {UDP_PORT} 端口，最多 {seconds} 秒（板子每 30 秒播报一次）…")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    except Exception:
        pass
    s.bind(("0.0.0.0", UDP_PORT))
    s.settimeout(seconds)
    try:
        data, addr = s.recvfrom(512)
    except socket.timeout:
        return None
    finally:
        s.close()
    return addr[0], data.decode("utf-8", "replace")


def peek_serial(seconds=4):
    """读串口里板子打印的 ip=…"""
    try:
        import serial
    except Exception:
        return None
    port = find_port()
    if not port:
        say("（没探到串口，跳过串口找 IP）")
        return None
    try:
        with serial.Serial(port, BAUD, timeout=0.3) as sp:
            say(f"🔌 串口 {port} 偷听 {seconds} 秒，找 IP …")
            t0 = time.time()
            while time.time() - t0 < seconds:
                line = sp.readline().decode("utf-8", "replace").strip()
                if line:
                    say("   " + line)
                m = None
                import re
                m = re.search(r"ip=(\d+\.\d+\.\d+\.\d+)", line) or \
                    re.search(r"http://(\d+\.\d+\.\d+\.\d+)/", line)
                if m:
                    return m.group(1)
    except Exception as e:
        say(f"（串口读不了：{e}）")
    return None


def cmd_find(args):
    st = load_state()
    ip = args.ip or st.get("ip")
    if ip:
        info = board_alive(ip)
        if info:
            say(f"✅ 历史 IP 还活着：{ip}  {info}")
            return ip
        say(f"（历史 IP {ip} 没应答，继续找）")

    if args.serial_only is False:
        hits = scan_subnet()
        if hits:
            ip, info = hits[0]
            say(f"✅ 扫到板子：{ip}  {info}")
            save_state(ip=ip, fw=(info or {}).get("fw"), ver=(info or {}).get("ver"))
            return ip

    ip = peek_serial(4)
    if ip:
        info = board_alive(ip)
        say(f"✅ 串口告诉我 IP：{ip}  {info}")
        save_state(ip=ip)
        return ip

    if args.wait > 0:
        got = listen_udp(args.wait)
        if got:
            ip, raw = got
            say(f"✅ 收到广播：{ip}  {raw}")
            save_state(ip=ip)
            return ip

    say("❌ 没找到板子。可能原因：①还没配网（跑 `portal` 看怎么配）②和电脑不在同一网段 ③板子没上电")
    return None


# ----------------------------------------------------------------- build
def write_stamp():
    stamp = time.strftime("%y%m%d-%H%M%S")
    sk_stamp().write_text(f'#define FW_BUILD "{stamp}"\n', encoding="utf-8")
    return stamp


def cmd_build(args):
    stamp = write_stamp()
    build = sk_build()
    build.mkdir(parents=True, exist_ok=True)
    say(f"📦 工程：{sk_name()}   产物目录：{build}")
    rc = run([cli_path(), *cli_cfg_args(), "compile",
              "--fqbn", FQBN,
              "--output-dir", build,
              sk_dir()]).returncode
    if rc != 0:
        die("编译失败（看上面报错）")
    b = sk_bin()
    size = b.stat().st_size if b.exists() else 0
    say(f"✅ 编译完成  build={stamp}  {b.name} {size} bytes")
    save_state(build=stamp, bin_size=size, sketch=sk_name())
    return stamp


# ----------------------------------------------------------------- push
def cmd_push(args):
    stamp = cmd_build(args)
    ip = cmd_find(argparse.Namespace(ip=args.ip, wait=8, serial_only=False))
    if not ip:
        die("找不到板子，没法推送")
    espota = espota_path()
    say(f"🚀 espota 推送 → {ip}:{OTA_PORT}   [{sk_name()}]   ({espota.parents[1].name})")
    rc = run([sys.executable, espota, "-i", ip, "-p", str(OTA_PORT),
              "-a", ota_pass(), "-f", sk_bin()]).returncode
    if rc != 0:
        die("espota 推送失败")

    say("⏳ 等板子重启并回报新固件…")
    t0 = time.time()
    while time.time() - t0 < 60:
        time.sleep(3)
        info = board_alive(ip, 2.0)
        if info and info.get("build") == stamp:
            st = board_status(ip) or {}          # /ping 没有 up/rssi，细节去 /status 拿
            say(f"✅ OTA 成功！板子现在跑 build={stamp}  ver={st.get('ver', info.get('ver'))}"
                f"  刚重启 up={st.get('up')}s  rssi={st.get('rssi')}")
            save_state(ip=ip, build=stamp)
            return True
        if info:
            say(f"   …还是旧固件（build={info.get('build')}），继续等")
    say("⚠️ 60 秒内没看到新 build 号。可能：板子没重启成功 / 串口日志要看一眼（ota.py serial）")
    return False


# ----------------------------------------------------------------- pull
def board_status(ip, timeout=2.0):
    """读 /status（含 up / build 字段）。注意 /ping 没有 up 字段，判定重启必须用这个。"""
    txt = http_get(f"http://{ip}/status", timeout=timeout)
    if not txt:
        return None
    try:
        return json.loads(txt)
    except Exception:
        return None


def cmd_pull(args):
    b = sk_bin()
    if not b.exists():
        die(f"还没编译 {sk_name()}，先跑 `ota.py build`")
    import shutil
    fw = sk_build() / "fw.bin"
    shutil.copyfile(b, fw)
    ip = cmd_find(argparse.Namespace(ip=args.ip, wait=8, serial_only=False))
    if not ip:
        die("找不到板子")

    before = board_status(ip) or {}
    before_up = int(before.get("up", 0))
    say(f"（拉之前：build={before.get('build')} up={before_up}s）")

    myip = local_ip()
    handler = lambda *a, **kw: SimpleHTTPRequestHandler(*a, directory=str(sk_build()), **kw)
    httpd = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), handler)
    import threading
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    say(f"📦 本机 HTTP 服务已起：http://{myip}:{HTTP_PORT}/fw.bin")

    url = f"http://{ip}/pull?host={myip}&port={HTTP_PORT}&path=/fw.bin"
    say(f"👉 让板子自己拉：{url}")
    say("   板子返回：" + str(http_get(url, 3)))
    say("⏳ 等它拉完重启…")
    t0 = time.time()
    while time.time() - t0 < 120:
        time.sleep(4)
        st = board_status(ip)
        if not st:
            continue                      # 正在刷/重启中，读不到很正常
        up = int(st.get("up", 99999))
        if up < before_up:                # uptime 倒退 = 它确实重启过了
            say(f"✅ 拉取式 OTA 完成，板子刚重启：build={st.get('build')} up={up}s")
            httpd.shutdown()
            return True
        say(f"   …还没重启（build={st.get('build')} up={up}s）")
    say("⚠️ 没等到重启，看串口日志确认")
    httpd.shutdown()
    return False


# ----------------------------------------------------------------- 其他
def cmd_status(args):
    ip = args.ip or load_state().get("ip")
    if not ip:
        ip = cmd_find(argparse.Namespace(ip=None, wait=0, serial_only=False))
    if not ip:
        die("找不到板子")
    txt = http_get(f"http://{ip}/status", 3)
    if not txt:
        die(f"{ip} 没应答 /status")
    try:
        data = json.loads(txt)
    except Exception:
        say(txt)
        return
    say(json.dumps(data, ensure_ascii=False, indent=2))


def cmd_serial(args):
    try:
        import serial
    except Exception:
        die("没有 pyserial（pip install pyserial）")
    port = args.port or find_port(required=True)
    say(f"📖 串口 {port} @ {BAUD}（Ctrl-C 停）")
    with serial.Serial(port, BAUD, timeout=0.5) as sp:
        t0 = time.time()
        while True:
            if args.sec and time.time() - t0 > args.sec:
                return
            line = sp.readline().decode("utf-8", "replace").rstrip()
            if line:
                print(f"[{time.strftime('%H:%M:%S')}] {line}", flush=True)


def cmd_ports(args):
    ports = list_ports()
    if not ports:
        say("🔌 没探到串口（插好线再试；Linux 还要确认有 /dev/ttyUSB* 权限，通常要加入 dialout 组）")
        return
    say(f"🔌 探到 {len(ports)} 个串口：")
    for p in ports:
        say(f"   {p['device']}   {p['desc']}")


def cmd_rescue(args):
    port = args.port or find_port(required=True)
    say(f"🆘 串口兜底烧录 → {port}")
    cmd_build(args)          # 总是重新编译：否则会把上一次的旧 bin 又刷一遍（踩过这个坑）
    rc = run([cli_path(), *cli_cfg_args(), "upload",
              "-p", port, "--fqbn", FQBN, "--input-dir", sk_build(), sk_dir()]).returncode
    if rc != 0:
        die("串口烧录失败")
    say("✅ 串口烧录完成")


def cmd_portal(args):
    say("""
📶 配网指引（板子进不了网时）
  1. 给板子通电（USB 或 5V），等 LED 开始「快闪」
  2. 手机/电脑连热点：OTABase-<芯片号>（开放热点，无密码）
  3. 浏览器打开：http://192.168.4.1/   （手机若弹"无网络"提示，选"仍然连接"）
  4. 选 WiFi + 填密码 → 保存并重启
  5. 回到电脑跑：python tools/ota.py find   → 应该能扫到
  备注：想重新配网，板子在 STA 模式下访问 http://<板子IP>/reset 即可清凭据重启
  备注：串口找不到时先看有哪些口 —— python tools/ota.py ports
        （Linux/macOS 例：/dev/ttyUSB0、/dev/ttyACM0；Windows 例：COM5）
""")


def cmd_use(args):
    d = FWROOT / args.name
    if not d.is_dir():
        die(f"没有这个工程目录：{d}")
    save_state(sketch=args.name)
    say(f"✅ 默认固件工程切到 「{args.name}」   ({d})")


# ----------------------------------------------------------------- main
def add_sketch_arg(p):
    p.add_argument("--sketch", help="固件工程名（firmware\\<名字>\\<名字>.ino）；缺省用上次用过的")


def main():
    global SKETCH_NAME
    ap = argparse.ArgumentParser(description="ESP8266 OTA 一键操作台（多工程：--sketch 或 use 切换）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("find", help="找板子")
    p.add_argument("--ip"); p.add_argument("--wait", type=int, default=0,
                                           help="没扫到时听 UDP 广播的秒数（板子每 30 秒播一次）")
    p.add_argument("--serial-only", action="store_true")
    p.set_defaults(func=cmd_find)

    p = sub.add_parser("build", help="编译")
    add_sketch_arg(p)
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("push", help="编译+OTA推送+校验")
    p.add_argument("--ip")
    add_sketch_arg(p)
    p.set_defaults(func=cmd_push)

    p = sub.add_parser("pull", help="拉取式 OTA")
    p.add_argument("--ip")
    add_sketch_arg(p)
    p.set_defaults(func=cmd_pull)

    p = sub.add_parser("status", help="板子状态")
    p.add_argument("--ip")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("serial", help="看串口日志")
    p.add_argument("--port"); p.add_argument("--sec", type=int, default=0)
    p.set_defaults(func=cmd_serial)

    p = sub.add_parser("ports", help="列出系统串口（自动探测用）")
    p.set_defaults(func=cmd_ports)

    p = sub.add_parser("rescue", help="串口兜底烧录")
    p.add_argument("--port")
    add_sketch_arg(p)
    p.set_defaults(func=cmd_rescue)

    p = sub.add_parser("use", help="切换默认固件工程")
    p.add_argument("name")
    p.set_defaults(func=cmd_use)

    p = sub.add_parser("portal", help="配网指引")
    p.set_defaults(func=cmd_portal)

    args = ap.parse_args()
    SKETCH_NAME = getattr(args, "sketch", None)     # 非 None 时覆盖 state
    args.func(args)


if __name__ == "__main__":
    main()
