#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_matrix.py —— rom-watch 多配置编译矩阵（证明编译开关真的省资源 ✓）

用法（在 serial-screen 目录下）：
    python tools\\build_matrix.py               跑全部配置，打印资源对比表
    python tools\\build_matrix.py --only minimal   只跑某一个
    python tools\\build_matrix.py --list        只列出配置名

为什么要它：IRAM 曾经 96%（63475/65536）⇒ 再加功能就编不过 ✗。
把"串口屏 / OLED"做成编译开关之后，**必须有一张实测表**证明省了多少，
否则开关只是个说法 ✓。产物落在 firmware\\build-matrix\\<配置名>\\（**刻意不碰 ota.py 的 build\\** ✓，
免得把要 OTA 推送的 bin 覆盖掉 ✗）。

★用 extra_flags 传 -D 覆盖，**不改 config.h** —— 免得把主人的 WiFi 凭据搞丢 ✗

★2026-09-30 加（主人「接着维护当前项目适配性系统 记得我这个项目要开源的」）：
  ① 新增 `oled32` 配置：验证 128x32 小屏那套版面也能编过 ✓
  ② 新增 `portable` 配置：**把三个"可公开"的文件复制到临时目录、不带 config.h 编一遍** ✓✓
     —— 这是"没有私有文件也能编"的**实测证据**（光靠注释说"能编"不算 ✓），
        也是开源前最重要的一条自检：别人 clone 下来直接就能编译 ✓
  ③ 板型可用环境变量覆盖：`set ROMWATCH_FQBN=esp8266:esp8266:nodemcuv2`
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent          # serial-screen\
CLI = ROOT / "tools" / "arduino-cli" / "arduino-cli.exe"
CFG = ROOT / "tools" / "arduino15" / "arduino-cli.yaml"
SKETCH = ROOT / "firmware" / "rom-watch"
OUT = ROOT / "firmware" / "build-matrix"
# ★板型可换（适配层的一部分）：换 ESP8266 开发板时用环境变量指定即可，不用改脚本 ✓
FQBN = os.environ.get("ROMWATCH_FQBN", "esp8266:esp8266:d1_mini")

# 可公开的三件套（不含 config.h / fw_build.h）—— portable 配置就用它们
PUBLIC_FILES = ["rom-watch.ino", "web_ui.h", "cn_font.h"]

VARIANTS = {
    "full":     [],                                   # 串口屏 + OLED（主人现在的用法）
    "no_tjc":   ["-DUSE_TJC=0"],                      # 没有串口屏
    "no_oled":  ["-DUSE_OLED=0"],                     # 没有 OLED
    "minimal":  ["-DUSE_TJC=0", "-DUSE_OLED=0"],      # 两个都没有（最小固件）
    "oled32":   ["-DOLED_SMALL=1"],                   # 128x32 小屏那套版面
    "portable": None,                                 # 特殊：不带 config.h 的干净副本
}

ROW = re.compile(r"used\s+(\d+)\s*/\s*(\d+)\s*bytes\s*\((\d+)%\)")


def make_portable_sketch() -> Path:
    """把"可公开的三件套"复制到临时目录（**故意不带 config.h**）⇒ 验证干净副本能编 ✓"""
    dst = OUT / "_portable" / "rom-watch"
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    for f in PUBLIC_FILES:
        shutil.copy2(SKETCH / f, dst / f)
    return dst


def compile_one(name: str, defs):
    outdir = OUT / name
    outdir.mkdir(parents=True, exist_ok=True)
    src = SKETCH
    flags = defs or []
    if defs is None:                                  # portable：干净副本 + 不传任何 -D
        src = make_portable_sketch()
        flags = []
    cmd = [str(CLI), "--config-file", str(CFG), "compile",
           "--fqbn", FQBN, "--output-dir", str(outdir), str(src)]
    if flags:
        cmd += ["--build-property", "compiler.cpp.extra_flags=" + " ".join(flags)]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    if p.returncode != 0:
        print("❌ %s 编译失败：%s" % (name, " ".join(flags) or "（默认）"))
        for line in text.splitlines():
            if "error" in line.lower():
                print("   " + line.strip())
        return None
    rows = ROW.findall(text)
    # 顺序固定是：RAM / IRAM / Flash（编译器就是按这个顺序打的 ✓）
    if len(rows) < 3:
        print("⚠️ %s 没解析到资源行，原始输出尾部：\n%s" % (name, "\n".join(text.splitlines()[-8:])))
        return None
    binfile = outdir / "rom-watch.ino.bin"
    return {
        "name": name,
        "defs": " ".join(flags) or ("（干净副本：无 config.h ✓）" if defs is None else "（默认）"),
        "ram": int(rows[0][0]), "ram_pct": int(rows[0][2]),
        "iram": int(rows[1][0]), "iram_pct": int(rows[1][2]),
        "flash": int(rows[2][0]), "flash_pct": int(rows[2][2]),
        "bin": binfile.stat().st_size if binfile.exists() else 0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=list(VARIANTS))
    ap.add_argument("--list", action="store_true", help="只列出配置名")
    args = ap.parse_args()

    if args.list:
        for k, v in VARIANTS.items():
            print("%-9s %s" % (k, "（干净副本）" if v is None else (" ".join(v) or "（默认）")))
        return

    print("板型 FQBN = %s   （可用环境变量 ROMWATCH_FQBN 覆盖 ✓）" % FQBN)
    todo = {args.only: VARIANTS[args.only]} if args.only else VARIANTS
    results = []
    for name, defs in todo.items():
        print("🔨 编译 %-9s %s" % (name, "（干净副本，无 config.h）" if defs is None else (" ".join(defs) or "（默认）")), flush=True)
        r = compile_one(name, defs)
        if r:
            results.append(r)
            print("   ✓ RAM %d (%d%%)  IRAM %d (%d%%)  Flash %d (%d%%)  bin %d B"
                  % (r["ram"], r["ram_pct"], r["iram"], r["iram_pct"],
                     r["flash"], r["flash_pct"], r["bin"]), flush=True)

    if not results:
        sys.exit(1)

    print("\n=== rom-watch 多配置资源对比（IRAM 是命门：上限 65536）===")
    print("%-9s %-26s %8s %8s %9s %10s" % ("配置", "编译开关", "RAM", "IRAM", "Flash", "bin 大小"))
    base = results[0]
    for r in results:
        print("%-9s %-26s %8d %8d %9d %10d%s" % (
            r["name"], r["defs"], r["ram"], r["iram"], r["flash"], r["bin"],
            "" if r is base else "   (IRAM %+d)" % (r["iram"] - base["iram"])))
    print("\n★结论怎么读：关掉哪个功能，IRAM 掉多少 —— 那一行就是它的真实成本 ✓")
    print("★portable 那行＝**别人 clone 下来（没有 config.h）也能编过**的实测证据 ✓✓")


if __name__ == "__main__":
    main()
