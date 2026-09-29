#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_publish.py —— serial-screen 的「发布前自检」（2026-09-30 建立）

它回答两个问题：
  ① **哪些文件会被提交进仓库**？—— 用**真 git 的 ignore 语义**算（不是自己写通配符猜 ✓）
  ② 这些文件里**有没有私有信息**？—— 个人绝对路径 / 内网 IP / 以及你写在
     `tools/.publish-secrets.txt`（**本地文件，不进仓库**）里的字面量（WiFi 名、密码、域名…）

用法（在 serial-screen 目录下）：
    python tools\\check_publish.py            自检；PASS 退出码 0 / FAIL 退出码 1
    python tools\\check_publish.py --list     顺便列出"会被提交的文件"清单
    python tools\\check_publish.py --secrets "foo" "bar"   临时加几个要查的字面量

★为什么要它：光靠"我记得把凭据挪走了"不算数 ✗ ——
  本喵 2026-09-30 的只读审计就发现：源码干净了，**编译产物 .bin 里照样是明文 WiFi 密码** ✗✓。
  所以这份自检**必须同时覆盖"会被提交的文件"和"被忽略但仍在磁盘上的危险产物"** ✓。

实现要点（都不改你的东西）：
  · 只做**只读**扫描；
  · 算 ignore 时在项目根临时 `git init` 一个空仓库（**不 add、不 commit**），跑完 `check-ignore`
    就**立刻删掉** `.git`（若本来就有 .git 则完全不碰 ✓）；
  · 关掉全局 excludes（`-c core.excludesFile=`）⇒ 结果和"别人 clone 下来"一致 ✓。
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
SECRETS_FILE = ROOT / "tools" / ".publish-secrets.txt"
SKIP_DIRS = {".git", "__pycache__", ".dsh-meow"}

# ---- 通用规则：**不含任何私有字面量**（这份脚本本身要能公开 ✓）----
GENERIC = [
    ("个人绝对路径", re.compile(r"[A-Za-z]:[\\/]Users[\\/][^\\/\r\n\"'<>|]+")),
    ("内网 IP",      re.compile(r"\b(?:192\.168|10|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b")),
    ("疑似密钥",      re.compile(r"\b(?:ghp_|gho_|sk-)[A-Za-z0-9]{16,}")),
    ("经纬度坐标",    re.compile(r"\d{1,3}\.\d{1,3}\s*°\s*[EWew]")),
]
# ★白名单：命中这些**不算泄露**（否则自检天天误报，最后没人看它 ✗）
# ★2026-09-30 收紧（独立审计指出"192.168.1.x 全放行"太宽 ✗）：只放**标准默认地址**，
#   文档里要举例就写 `<板子的IP>` —— 别用真实网段的字面量 ✓
ALLOW = [
    (re.compile(r"^192\.168\.4\.1$"), "ESP8266 AP 配网的默认地址（标准值，不是私有信息 ✓）"),
    (re.compile(r"^192\.168\.1\.1$"), "家用路由器最常见的默认网关（通用举例 ✓）"),
    (re.compile(r"^116\.40\s*°\s*E$"), "北京的通用示例坐标（文档里举例用 ✓）"),
    (re.compile(r"^39\.90\s*°\s*N$"), "北京的通用示例坐标（文档里举例用 ✓）"),
]


def is_allowed(frag: str) -> bool:
    return any(rx.match(frag) for rx, _ in ALLOW)

# ---- 必须被 .gitignore 挡住的"危险文件"（存在即视为隐患）----
MUST_IGNORE = [
    "firmware/rom-watch/config.h",
    "firmware/ota-state.json",
    "tools/.publish-secrets.txt",
]
MUST_IGNORE_GLOB = [
    ("firmware/build", "编译产物目录"),
    ("firmware/build-matrix", "多配置编译产物目录"),
]
MUST_IGNORE_SUFFIX = [
    (".bin", "编译产物（★里面是明文凭据）"),
    (".elf", "编译产物"),
    (".map", "编译产物"),
    (".bak", "旧版快照（含旧凭据）"),
]

BIN_SUFFIX = {".bin", ".elf", ".map", ".tft"}
# ★2026-09-30 加（独立审计的核心发现）：原来 TEXT/BIN 两个集合都不含图片 ⇒
#   提交集里 8 张图**一张都不扫** ✗ —— 而桌面全屏截图（会话名/PID/个人壁纸）恰恰是最严重的泄露 ✗✓。
#   现在：①图片也进扫描（搜原始字节，能抓到 tEXt 之类的元数据）
#        ②★**像素里渲染出来的文字脚本永远搜不到** ⇒ 用一条**结构性规则**兜住：
#          **仓库自己的配图只许放 `docs/`**，别处的图片一律算隐患 ✓（那条规则正好抓住桌面截图 ✓）
IMG_SUFFIX = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico", ".svg"}
TEXT_SUFFIX = {".ino", ".h", ".hpp", ".c", ".cpp", ".py", ".js", ".cjs", ".md", ".txt",
               ".json", ".ps1", ".bat", ".sh", ".yml", ".yaml", ".gitignore", ""}


def load_secrets(extra: list[str]) -> list[str]:
    out = list(extra)
    if SECRETS_FILE.exists():
        for line in SECRETS_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
            s = line.strip()
            if s and not s.startswith("#"):
                out.append(s)
    return out


def iter_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for f in filenames:
            yield Path(dirpath) / f


def git_ignored(paths: list[Path]) -> set[Path]:
    """用真 git 的 ignore 语义算出被忽略的那些（临时建仓，跑完即删 ✓）"""
    git = shutil.which("git")
    if not git:
        print("⚠️ 没找到 git ⇒ 只能按文件名粗判，结果仅供参考 ✗")
        return set()
    made = not (ROOT / ".git").exists()
    try:
        if made:
            subprocess.run([git, "init", "-q"], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rel = [str(p.relative_to(ROOT)).replace("\\", "/") for p in paths]
        p = subprocess.run(
            [git, "-c", "core.excludesFile=", "check-ignore", "--stdin", "-z"],
            cwd=ROOT, input="\0".join(rel), text=True, capture_output=True)
        got = [x for x in (p.stdout or "").split("\0") if x]
        return {ROOT / g for g in got}
    except Exception as e:
        print("⚠️ git check-ignore 失败：%s ⇒ 按「不忽略」保守处理 ✗" % e)
        return set()
    finally:
        if made:
            shutil.rmtree(ROOT / ".git", ignore_errors=True)


def scan_text(path: Path, pats):
    hits = []
    try:
        data = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return hits
    for i, line in enumerate(data.splitlines(), 1):
        for name, rx in pats:
            m = rx.search(line)
            if m and not is_allowed(m.group(0)):
                hits.append((path, i, name, m.group(0)[:60]))
    return hits


def scan_bin(path: Path, secrets: list[str], secrets_only: bool = False):
    hits = []
    try:
        blob = path.read_bytes()
    except Exception:
        return hits
    for s in secrets:
        if s.encode("utf-8", errors="ignore") in blob:
            hits.append((path, 0, "字面量 %r" % s, "(二进制里命中)"))
    if secrets_only:
        return hits                              # 图片：只搜字面量，别拿通用正则去误报像素 ✗
    for name, rx in GENERIC:
        rxb = re.compile(rx.pattern.encode())     # ★正则得用 bytes 版，别拿 str 模式喂 bytes ✗
        m = rxb.search(blob)
        if m:
            frag = m.group(0).decode("utf-8", "replace")
            if not is_allowed(frag):
                hits.append((path, 0, name, frag[:60]))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="列出会被提交的文件")
    ap.add_argument("--secrets", nargs="*", default=[], help="额外要查的字面量")
    args = ap.parse_args()

    print("=" * 74)
    print("serial-screen 发布前自检   根目录 = %s" % ROOT)
    print("=" * 74)

    allf = [p for p in iter_files()]
    ignored = git_ignored(allf)
    tracked = sorted([p for p in allf if p not in ignored])

    print("\n【1】文件账")
    print("    磁盘上共 %d 个文件；会被提交 %d 个；被 .gitignore 挡住 %d 个"
          % (len(allf), len(tracked), len(ignored)))
    if args.list:
        for p in tracked:
            print("      + " + str(p.relative_to(ROOT)).replace("\\", "/"))

    secrets = load_secrets(args.secrets)
    pats = list(GENERIC) + [("私有字面量 %r" % s, re.compile(re.escape(s))) for s in secrets]
    print("    私有字面量来源：%s（%d 条）"
          % (("tools/.publish-secrets.txt" if SECRETS_FILE.exists() else "（无本地清单）"), len(secrets)))

    print("\n【2】危险文件有没有被挡住")
    ok2 = True
    for rel in MUST_IGNORE:
        p = ROOT / rel
        state = "不存在" if not p.exists() else ("已忽略 ✓" if p in ignored else "★没被忽略 ✗")
        if p.exists() and p not in ignored:
            ok2 = False
        print("    %-34s %s" % (rel, state))
    for sub, why in MUST_IGNORE_GLOB:
        p = ROOT / sub
        if not p.exists():
            print("    %-34s 不存在" % (sub + "/"))
            continue
        inside = [f for f in allf if f.is_relative_to(p)]
        leaked = [f for f in inside if f not in ignored]
        print("    %-34s %s（%d 个文件）%s"
              % (sub + "/", why, len(inside), "已忽略 ✓" if not leaked else "★有 %d 个漏网 ✗" % len(leaked)))
        if leaked:
            ok2 = False
            for f in leaked[:5]:
                print("        ★没被忽略：%s" % f.relative_to(ROOT))
    bad_suffix = [f for f in tracked if f.suffix.lower() in dict(MUST_IGNORE_SUFFIX)]
    print("    %-34s %s" % ("*.bin/*.elf/*.map/*.bak", "无 ✓" if not bad_suffix else "★有 %d 个没被忽略 ✗" % len(bad_suffix)))
    if bad_suffix:
        ok2 = False
        for f in bad_suffix[:5]:
            print("        ★%s" % f.relative_to(ROOT))

    print("\n【3】「会被提交的文件」里有没有私有信息")
    hits = []
    imgs = []
    for p in tracked:
        suf = p.suffix.lower()
        if suf in TEXT_SUFFIX:
            hits += scan_text(p, pats)
        elif suf in BIN_SUFFIX:
            hits += scan_bin(p, secrets)
        elif suf in IMG_SUFFIX:
            imgs.append(p)
            hits += scan_bin(p, secrets, secrets_only=True)   # 图片：只搜字面量（元数据里也可能有 ✓）
    # ★图片的"像素里的文字"脚本搜不到 ⇒ 用规则兜：**仓库自己的配图只许放 docs/** ✓
    stray = [p for p in imgs if "docs" not in p.relative_to(ROOT).parts]
    ok3 = (not hits) and (not stray)
    if not hits:
        print("    字面量/规则：零命中 ✓")
    else:
        for p, ln, name, frag in hits[:40]:
            print("    ★%s:%s  [%s]  %s" % (p.relative_to(ROOT), ln or "-", name, frag))
        if len(hits) > 40:
            print("    …还有 %d 处" % (len(hits) - 40))
    if stray:
        print("    ★有 %d 张图片**不在 docs/ 里** ⇒ 截图/照片里可能带私有内容"
              "（脚本搜不出像素里的字 ✗，只能靠这条规则兜 ✓）：" % len(stray))
        for p in stray[:8]:
            print("        %s" % p.relative_to(ROOT))
    if imgs:
        print("    ⚠️ 提交集里有 %d 张图片 ⇒ **必须人工目视一遍**（脚本对图里的文字结构性失明）："
              % len(imgs))
        print("        " + "、".join(str(p.relative_to(ROOT)) for p in imgs[:8]))

    print("\n【4】被忽略、但**仍在磁盘上**的危险产物（提醒：它们已经存在过）")
    warn = [f for f in ignored if f.suffix.lower() in BIN_SUFFIX]
    dirty = []
    for f in warn:
        h = scan_bin(f, secrets)
        if h:
            dirty.append((f, len(h)))
    print("    扫了 %d 个产物（.bin/.elf/.map/.tft），其中 %d 个**真的含私有字面量**：" % (len(warn), len(dirty)))
    if dirty:
        for f, n in sorted(dirty, key=lambda x: -x[1])[:10]:
            print("      %s  ← %d 处" % (f.relative_to(ROOT), n))
        if len(dirty) > 10:
            print("      …共 %d 个" % len(dirty))
        print("    ★这些文件**只在本机**、已被 gitignore ⇒ 不会进仓库 ✓；")
        print("      但它们曾经存在/可能外发过 ⇒ 若你介意，换掉 WiFi 密码与 OTA 口令最稳妥 ✓")
    else:
        print("      （一个都没有 ✓）")

    ok = ok2 and ok3
    print("\n" + "=" * 74)
    print("结论：%s" % ("✅ PASS —— 可以公开（上面【4】那几条自己心里有数就行）" if ok
                       else "❌ FAIL —— 先把上面带 ★ 的处理掉再公开"))
    print("=" * 74)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
