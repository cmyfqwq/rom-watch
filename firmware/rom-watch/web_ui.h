/*
 * web_ui.h —— rom-watch 的网页资源（页头小样式 + 主页面三段 + 配网页样式）
 * ============================================================================
 * ★★为什么这些字面量必须放在 **.h** 而不是 .ino（踩过的坑，别搬回去 ✗）：
 *   arduino-cli 编译 .ino 前会做「自动插入函数原型」：它拿 **ctags** 去扫 .ino，
 *   而 **ctags 看不懂 C++ 原始字符串** ✗ —— 于是 JS 里的
 *        function esc(s){ … }
 *   被它当成 C++ 函数定义，在上头插了一条 `function esc(s);` ⇒
 *        rom-watch.ino:1512:1: error: 'function' does not name a type
 *   ctags 只扫 .ino / .cpp，**不扫 .h** ⇒ 网页资源放这里就彻底免疫 ✓✓
 * ============================================================================
 */
#pragma once
#include <Arduino.h>

static const char MINI_CSS[] PROGMEM = R"MCSS(
*{box-sizing:border-box}
body{margin:0;padding:22px 16px calc(28px + env(safe-area-inset-bottom));
font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,Roboto,"PingFang SC","Microsoft YaHei",sans-serif;
color:#101a2e;background:linear-gradient(180deg,#eef2f9,#e7ecf5);min-height:100vh;
-webkit-text-size-adjust:100%}
.w{max-width:560px;margin:0 auto;background:#fff;border:1px solid rgba(15,23,42,.08);border-radius:16px;
padding:22px;box-shadow:0 8px 28px rgba(15,23,42,.08)}
h1{margin:0 0 4px;font-size:22px;letter-spacing:.2px}
p.h{color:#5b6b85;font-size:13px;margin:0 0 16px}
label{display:block;font-size:12.5px;color:#5b6b85;margin:12px 0 5px}
input,select{width:100%;min-height:46px;padding:11px 13px;font:inherit;color:#101a2e;background:#f7f9fd;
border:1px solid rgba(15,23,42,.12);border-radius:11px}
input:focus,select:focus{outline:none;border-color:#0d9488;box-shadow:0 0 0 3px rgba(13,148,136,.15)}
button{font:inherit;font-weight:650;min-height:46px;padding:12px 18px;border-radius:11px;border:0;
background:#0d9488;color:#fff;cursor:pointer;margin-top:18px;width:100%}
button:hover{filter:brightness(1.06)}button:active{transform:translateY(1px)}
a{color:#0284c7}
.msg{border-left:4px solid #0d9488;background:#f0fdfa;border-radius:10px;padding:12px 14px;margin:0 0 14px}
)MCSS";

String pageHeader(const String &title) {
  String h = F("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
               "<meta name='color-scheme' content='dark light'><title>");
  h += title;
  h += F("</title><style>");
  h += FPSTR(MINI_CSS);              // ★FPSTR：让 String 从 flash 读，而不是把 flash 指针当 RAM 解引用 ✗
  h += F("</style></head><body><div class='w'>");
  return h;
}

/* ---------- 主页面：三段 PROGMEM，用 chunked 一段段推出去 ---------- */
static const char UI_HEAD[] PROGMEM = R"HTMLUI(<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<meta name="theme-color" content="#0b1020">
<meta name="description" content="rom-watch · 系统包更新监测面板">
<title>rom-watch · 系统包监测</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='8' fill='%230b1020'/%3E%3Cpath d='M9 22V10h6.4a4 4 0 010 8H9' stroke='%235eead4' stroke-width='2.6' fill='none' stroke-linecap='round'/%3E%3Ccircle cx='23' cy='22' r='2' fill='%235eead4'/%3E%3C/svg%3E">
<style>
*{box-sizing:border-box}
:root{
  --bg:#0b1020;--bg2:#0f172c;--panel:#131b30;--panel2:#182142;--line:rgba(255,255,255,.10);
  --fg:#e9eefb;--mut:#93a1bb;--acc:#5eead4;--acc2:#7dd3fc;
  --ok:#34d399;--warn:#fbbf24;--bad:#f87171;--r:14px;--sh:0 10px 30px rgba(0,0,0,.34)
}
@media (prefers-color-scheme:light){
  :root{--bg:#eef2f9;--bg2:#e7ecf5;--panel:#fff;--panel2:#f4f7fd;--line:rgba(15,23,42,.10);
  --fg:#101a2e;--mut:#5b6b85;--acc:#0d9488;--acc2:#0284c7;--ok:#059669;--warn:#b45309;--bad:#dc2626;
  --sh:0 6px 22px rgba(15,23,42,.08)}
}
html{-webkit-text-size-adjust:100%}
body{margin:0;padding:0 14px calc(26px + env(safe-area-inset-bottom));color:var(--fg);
  font:16px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,Roboto,"PingFang SC","Microsoft YaHei",sans-serif;
  background:radial-gradient(1200px 620px at 12% -12%,var(--bg2),var(--bg)) fixed}
/* ★大屏适配：1040 → 1600。挂大显示器/电视/壁挂屏时不再两边空一大片，
 *   配合「≥1400px 仍保持 2 列」的策略 ⇒ 每张卡都更宽（而不是硬凑第 3 列留空洞）✓ */
.wrap{max-width:1600px;margin:0 auto}
.bar{display:flex;align-items:center;gap:12px;padding:20px 2px 14px}
.mark{width:44px;height:44px;flex:0 0 44px;border-radius:13px;display:grid;place-items:center;color:var(--acc);
  background:linear-gradient(150deg,var(--panel2),var(--panel));border:1px solid var(--line);box-shadow:var(--sh)}
.mark svg{width:25px;height:25px}
.bar-txt{min-width:0;flex:1 1 auto}
h1{margin:0;font-size:19px;font-weight:700;letter-spacing:.2px}
.sub{margin:2px 0 0;color:var(--mut);font-size:12.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.live{margin-left:auto;flex:0 0 auto;display:flex;align-items:center;gap:7px;font-size:12.5px;color:var(--mut);
  background:var(--panel);border:1px solid var(--line);border-radius:999px;padding:6px 12px;white-space:nowrap}
.dot{width:8px;height:8px;flex:0 0 8px;border-radius:50%;background:var(--mut);transition:background .3s}
.live.on .dot{background:var(--ok);animation:pulse 2.4s infinite}
.live.off .dot{background:var(--bad)}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(52,211,153,.5)}70%{box-shadow:0 0 0 9px rgba(52,211,153,0)}100%{box-shadow:0 0 0 0 rgba(52,211,153,0)}}
/* ★「别参差不齐」的排版纪律（2026-09-29 主人指正后改）：
 *  ① 卡片栅格**不用 auto-fit** ✗ —— 它会按宽度自己决定列数，于是出现"第三列空着""某张卡单独占一行"的参差 ✗；
 *     改成**固定的 2 列**（≥760px），并且**行内卡片等高**（align-items:stretch ✓ 默认值）
 *  ② 一行里放几张卡是**排好的**：设备状态(宽) → 监测设置 | 启动黑匣子 → 最近6次检测(宽) ⇒ 每行都占满 ✓
 *  ③ 卡片内的小块用 **container query**（按卡片宽度，而不是窗口宽度）决定 2 列还是 4 列 ⇒
 *     手机 2×2、宽卡片 4×1，**永远整除、永远没有孤块** ✓ */
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);box-shadow:var(--sh);padding:18px;margin-top:14px;container-type:inline-size}
h2{margin:0 0 12px;font-size:12.5px;font-weight:700;letter-spacing:.9px;text-transform:uppercase;color:var(--mut)}
/* ★hero 左半边一定要 min-width:0（字号放大到 1.3 档时踩到的坑 ✗）：
 *   左半边的 #heroSub 用的是 .sub（white-space:nowrap）⇒ 它的 min-content 有 259px，
 *   而 `1fr` 的隐含最小值是 auto ⇒ 整条轨道被撑到 259px，卡片右边被顶破（按钮/文字溢出卡框）✗
 *   改成 minmax(0,1fr) + min-width:0 ⇒ 轨道只认容器宽，长文本交给 .sub 自己的省略号 ✓ */
.hero{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:20px;align-items:center;border-left:4px solid var(--ok);
  transition:border-color .3s ease}
.hero>div:first-child{min-width:0}
/* 同上：hero 的这行说明宁可折成两行，也别被省略号吃掉（'连续失败' 这类信息不能悄悄看不见）✓ */
.hero>div:first-child .sub{white-space:normal;overflow:visible;text-overflow:clip}
.hero[data-state=new]{border-left-color:var(--bad)}
.hero[data-state=idle]{border-left-color:var(--warn)}
.hero[data-state=offline]{border-left-color:var(--mut)}
.badge{display:inline-flex;align-items:center;gap:7px;font-size:12.5px;font-weight:650;padding:5px 12px;
  border-radius:999px;background:rgba(52,211,153,.14);color:var(--ok);border:1px solid rgba(52,211,153,.34)}
.badge.new{background:rgba(248,113,113,.14);color:var(--bad);border-color:rgba(248,113,113,.38)}
.badge.idle{background:rgba(251,191,36,.14);color:var(--warn);border-color:rgba(251,191,36,.34)}
.badge.offline{background:transparent;color:var(--mut);border-color:var(--line)}
.ver{font-size:clamp(30px,7.5vw,44px);font-weight:700;letter-spacing:-.6px;font-variant-numeric:tabular-nums;
  margin:8px 0 2px;overflow-wrap:anywhere}
.ringwrap{display:flex;flex-direction:column;align-items:center;gap:8px;min-width:96px}
.ring{position:relative;width:88px;height:88px}
.ring i{position:absolute;inset:0;border-radius:50%;transition:background .9s linear;
  background:conic-gradient(var(--acc) calc(var(--p,0) * 1%),var(--line) 0);
  -webkit-mask:radial-gradient(farthest-side,transparent 67%,#000 68%);
  mask:radial-gradient(farthest-side,transparent 67%,#000 68%)}
.ring span{position:absolute;inset:0;display:grid;place-items:center;font-size:14px;font-variant-numeric:tabular-nums;color:var(--mut)}
.ringcap{font-size:11.5px;color:var(--mut);margin:0;text-align:center;white-space:nowrap}
/* 按钮一律走栅格 ⇒ **等宽、整除**，不会出现"3 个一行 + 1 个掉到第二行" ✗
 *   默认（hero，4 个动作）：手机 2×2 ✓ ／ 宽卡片 4×1 ✓
 *   .act.two（保存/放弃，2 个）：永远 2 等分 ✓
 *   .act.three（维护，3 个）：手机竖排 3×1 ✓ ／ 宽卡片 3×1 ✓ —— 两种都整除 ✓ */
.act{display:grid;gap:10px;grid-template-columns:repeat(2,minmax(0,1fr));margin-top:16px}
@container (min-width:520px){.act{grid-template-columns:repeat(4,minmax(0,1fr))}}
.act.two{grid-template-columns:repeat(2,minmax(0,1fr))}
.act.three{grid-template-columns:repeat(1,minmax(0,1fr))}
@container (min-width:420px){.act.three{grid-template-columns:repeat(3,minmax(0,1fr))}}
.act button{width:100%}
button{font:inherit;font-weight:650;min-height:44px;padding:11px 16px;border-radius:11px;cursor:pointer;
  color:var(--fg);background:var(--panel2);border:1px solid var(--line);
  transition:transform .14s ease,background .2s ease,border-color .2s ease,opacity .2s ease}
button:hover{background:var(--panel);border-color:var(--acc)}
button:active{transform:translateY(1px)}
button:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
button[disabled]{opacity:.5;cursor:progress}
button.pri{background:var(--acc);color:#06251f;border-color:transparent}
button.dan{color:var(--bad);border-color:rgba(248,113,113,.42)}
button.gh{background:transparent}
button.sm{min-height:36px;padding:6px 12px;font-size:13px;font-weight:600}
.grid{display:grid;gap:14px;grid-template-columns:1fr}
@media (min-width:760px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
/* ★≥1400px 大屏：**故意还是 2 列**（不是 3 列）——理由：
 *   中间那排只有【监测设置】【启动黑匣子】两张非 wide 卡，栅格改成 3 列会空出第 3 格
 *   ⇒ 出现「半排空列」✗，而卡片总数又不够把 3 列填满（wide 卡跨满、不会补位）。
 *   按「宁可更宽，绝不留空洞」的原则：列数保持 2，靠 .wrap 放宽到 1600px 把每张卡摊宽 ✓
 *   每行仍然严格占满、行内等高、无孤块（2+1）✓ */
@media (min-width:1400px){
  .grid{gap:16px}
  .hero{padding:22px 26px;gap:30px}
  .ring{width:104px;height:104px}
  .ver{font-size:clamp(38px,3.6vw,60px)}
  .ringcap{font-size:12.5px}
}
.grid>.card{margin-top:0}
.wide{grid-column:1/-1}
/* 小块：**按卡片宽度**（container query）决定 2 列还是 4 列 —— 窗口宽度管不着卡片实际有多宽 ✓
 *   ⇒ 手机 2×2 ✓、宽卡片 4×1 ✓，**永远整除、没有孤块** ✗→✓ */
.tiles{display:grid;gap:10px;grid-template-columns:repeat(2,minmax(0,1fr))}
@container (min-width:430px){.tiles{grid-template-columns:repeat(4,minmax(0,1fr))}}
.tile{background:var(--panel2);border:1px solid var(--line);border-radius:12px;padding:12px}
.tile small{display:block;color:var(--mut);font-size:11.5px;letter-spacing:.3px}
.tile b{display:block;font-size:19px;font-weight:700;font-variant-numeric:tabular-nums;margin-top:3px;overflow-wrap:anywhere}
dl.kv{display:grid;grid-template-columns:auto minmax(0,1fr);gap:7px 14px;margin:14px 0 0;font-size:13.5px}
/* 宽卡片里把键值表排成两列，省得一行行拖得很长（窄卡片保持单列 ✓） */
@container (min-width:560px){dl.kv{grid-template-columns:auto minmax(0,1fr) auto minmax(0,1fr);column-gap:22px}}
/* ★字号放大档专用（容器实测 <300px，只有 ?scale≥1.2 的窄卡片会命中）：
 *   zoom 把卡片压窄后，键值表右边那列只剩几十 px ⇒ IP/设备名/MAC 会被 overflow-wrap
 *   拆成「192.16 / 8.110 / 115」这种碎片（不算溢出，但没法看）✗
 *   阈值压到 300px ⇒ 只在放大档改成「标签一行、值一行」（值拿到整行宽，一行放得下 ✓），
 *   普通手机（卡片 322px）与桌面（宽卡片）保持原样、一点不动 ✓ */
@container (max-width:300px){
  dl.kv{grid-template-columns:minmax(0,1fr);gap:4px 0}
  dl.kv dt{margin-top:8px}
  dl.kv dd{text-align:left}
}
dl.kv dt{color:var(--mut)}
dl.kv dd{margin:0;text-align:right;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}
.chips{display:flex;flex-wrap:wrap;gap:7px;margin-top:12px}
.chip{font-size:12px;font-weight:600;padding:4px 10px;border-radius:999px;border:1px solid var(--line);color:var(--mut)}
.chip.y{color:var(--ok);border-color:rgba(52,211,153,.34);background:rgba(52,211,153,.10)}
.chip.n{color:var(--bad);border-color:rgba(248,113,113,.34);background:rgba(248,113,113,.10)}
table{width:100%;border-collapse:collapse;font-size:13.5px}
th{text-align:left;color:var(--mut);font-weight:600;font-size:11.5px;letter-spacing:.5px;padding:6px 8px;
  border-bottom:1px solid var(--line);text-transform:uppercase}
td{padding:9px 8px;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums;white-space:nowrap}
/* ★2026-09-30 修（手机上"抓到/来源"两列被拆成两行 ✗）：原来 td 是 `overflow-wrap:anywhere`
 *   ⇒ 窄屏时连 `v4.0.21` 都能从中间断成 "v4.0.2 / 1" ✗（主人最烦这种参差 ✓）。
 *   现在：单元格**一律不折行** + 时间列在 JS 里缩成 `MM-DD HH:MM`（完整时刻放 title 里 ✓）
 *   ⇒ 390px 手机上一行正好放下 4 列 ✓；万一还窄，外面那层 .tw 允许横向滚动（保底不挤 ✗）*/
.tw{overflow-x:auto;-webkit-overflow-scrolling:touch}
tr:last-child td{border-bottom:0}
.ok{color:var(--ok);font-weight:650}.bad{color:var(--bad);font-weight:650}
.bars{display:inline-flex;align-items:flex-end;gap:3px;height:15px;vertical-align:-3px;margin-left:6px}
.bars i{width:4px;border-radius:1.5px;background:var(--mut);opacity:.3}
.bars i.on{background:var(--acc);opacity:1}
label{display:block;font-size:12.5px;color:var(--mut);margin:12px 0 5px}
input,select{width:100%;min-height:44px;padding:10px 13px;font:inherit;color:var(--fg);background:var(--bg2);
  border:1px solid var(--line);border-radius:11px}
input:focus,select:focus{outline:none;border-color:var(--acc);box-shadow:0 0 0 3px rgba(94,234,212,.16)}
/* 监测设置里的"高级"折叠块：默认收起，标题小一号，别把常用项挤下去 ✓ */
details.sub2{border:1px dashed var(--line);border-radius:11px;padding:8px 12px;margin-top:12px}
details.sub2>summary{cursor:pointer;font-size:12.5px;color:var(--mut);font-weight:600}
details.sub2[open]>summary{margin-bottom:6px}
/* ★电视/遥控可达性：所有可聚焦元素（按钮/链接/输入/折叠标题）都要有看得见的描边 ——
 *   放在 input:focus 之后（同特异性、后者生效）⇒ 键盘 Tab/方向键走到哪一眼就能看见 ✓
 *   a/summary 的默认虚线圈在电视远看几乎不可见，这里统一成 2px 实线 ✓ */
a:focus-visible,summary:focus-visible,input:focus-visible,select:focus-visible{
  outline:2px solid var(--acc);outline-offset:2px;border-radius:8px}
details.card>summary{cursor:pointer;font-size:13.5px;font-weight:650;color:var(--mut);list-style:none}
details.card>summary::-webkit-details-marker{display:none}
details.card>summary:before{content:"▸ ";color:var(--acc)}
details.card[open]>summary:before{content:"▾ "}
details.card[open]>summary{margin-bottom:12px}
pre.box{margin:0 0 10px;background:var(--bg2);border:1px solid var(--line);border-radius:12px;padding:12px;
  overflow:auto;max-height:240px;white-space:pre-wrap;word-break:break-all}
pre.box:last-child{margin-bottom:0}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace;font-size:12.5px;line-height:1.5}
.hint{font-size:12.5px;color:var(--mut);margin:10px 0 0}
.hint b{color:var(--fg)}
a{color:var(--acc2);text-decoration:none}
a:hover{text-decoration:underline}
.foot{display:grid;gap:10px;margin:20px 2px 0;font-size:12.5px;color:var(--mut)}
.foot .links{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
@media (min-width:640px){.foot .links{grid-template-columns:repeat(6,minmax(0,1fr))}}
.foot a{color:var(--acc2);border:1px solid var(--line);background:var(--panel);border-radius:999px;padding:6px 10px;
  text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.foot a:hover{text-decoration:none;border-color:var(--acc)}
.foot button{justify-self:start;width:auto}
.toasts{position:fixed;left:50%;transform:translateX(-50%);bottom:calc(18px + env(safe-area-inset-bottom));
  display:flex;flex-direction:column;gap:8px;z-index:9;pointer-events:none;max-width:92vw}
.toast{background:var(--panel);border:1px solid var(--line);border-left:3px solid var(--acc);border-radius:11px;
  padding:10px 14px;font-size:13.5px;box-shadow:var(--sh);animation:up .26s ease}
.toast.bad{border-left-color:var(--bad)}
@keyframes up{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
/* ===== ★「怎么用（3 步）」人话说明卡（默认展开、标题可点收起）===== */
ol.steps{margin:12px 0 0;padding-left:22px;display:grid;gap:10px;font-size:13.5px;line-height:1.6}
ol.steps li::marker{color:var(--acc);font-weight:700}
ol.steps b,ol.steps strong{color:var(--fg)}
/* 技术黑话的人话小注：塞进标题后面（h2 是 uppercase，这里要压回去）*/
.jr{font-size:11.5px;font-weight:400;color:var(--mut);letter-spacing:.2px;text-transform:none}

/* ===== ★页脚「字号」控件（− / ＋，写回 URL 的 ?scale=，不整页刷新）===== */
.footctl{display:flex;flex-wrap:wrap;gap:10px;align-items:center}
.zctl{display:inline-flex;align-items:center;gap:6px;color:var(--mut);font-size:12.5px}
.zctl button{min-height:32px;padding:4px 12px;font-size:15px;line-height:1;width:auto}
.zctl b{color:var(--fg);font-variant-numeric:tabular-nums;min-width:46px;text-align:center;font-size:12.5px}

/* ===== ★看板模式 ?kiosk=1（挂墙上/电视上远看）=====
 * 只留：hero（状态徽章＋版本大字＋倒计时环＋【立即检查】）＋设备状态里的 4 个小块＋最近 6 次检测
 * 隐藏：顶部标题栏、监测设置卡、启动黑匣子卡、调试/维护折叠区、怎么用卡、footer、hero 里的其它动作按钮
 * 注：设备状态卡与最近检测卡都是 .wide，中间两张窄卡是 .card ⇒ 一句 :not(.wide) 正好选准 ✓ */
.kexit{display:none}
body.kiosk{padding:0 18px 18px}
body.kiosk .bar,
body.kiosk details.card,
body.kiosk .foot{display:none}
body.kiosk .grid>section.card:not(.wide){display:none}
body.kiosk .grid>section.card.wide dl.kv{display:none}
body.kiosk .hero{margin-top:16px;padding:24px 26px;gap:26px}
body.kiosk .hero .hint{display:none}
body.kiosk .hero .act{display:block;margin-top:16px;max-width:260px}
body.kiosk .hero .act button:not(#bCheck){display:none}
body.kiosk .hero .act button{width:100%}
body.kiosk .badge{font-size:clamp(14px,1.2vw,20px);padding:7px 16px}
body.kiosk .ver{font-size:clamp(46px,6.4vw,120px);letter-spacing:-1.5px}
body.kiosk .ring{width:clamp(112px,10vw,180px);height:clamp(112px,10vw,180px)}
body.kiosk .ring span{font-size:clamp(17px,1.5vw,30px)}
body.kiosk .ringcap{font-size:clamp(12px,1vw,18px)}
body.kiosk h2{font-size:clamp(12.5px,1vw,17px)}
body.kiosk .tile b{font-size:clamp(20px,1.8vw,36px)}
body.kiosk .tile small{font-size:clamp(11.5px,.95vw,16px)}
body.kiosk table{font-size:clamp(13.5px,1.1vw,19px)}
body.kiosk th{font-size:clamp(11.5px,.9vw,15px)}
body.kiosk .kexit{display:block;margin:16px 2px 0}
.kexit a{color:var(--mut);font-size:12.5px;opacity:.5;border:0;background:none;padding:4px 2px}
.kexit a:hover,.kexit a:focus-visible{opacity:1;text-decoration:underline}
@media (max-width:560px){
  .hero{grid-template-columns:minmax(0,1fr);gap:14px}
  .ringwrap{flex-direction:row;justify-content:flex-start;gap:14px}
  .ring{width:66px;height:66px}
}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style></head><body>
<script>/* ★尽早生效：看板/字号要在首帧前定下来，否则大屏上会先闪一下完整版面 ✗（只读 URL，不碰数据）*/
(function(){var p=new URLSearchParams(location.search),s=parseFloat(p.get('scale'));
if(!(s>=0.8&&s<=1.6))s=1;document.documentElement.style.zoom=String(s);
if(p.get('kiosk')==='1')document.body.className='kiosk';})();</script>
)HTMLUI";

static const char UI_BODY[] PROGMEM = R"HTMLBODY(
<div class="wrap">
<header class="bar">
  <div class="mark" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none"><path d="M4.5 18V6.5h5.6a3.2 3.2 0 010 6.4H4.5" stroke="currentColor" stroke-width="2.1" stroke-linecap="round"/><circle cx="17.6" cy="17.4" r="1.7" fill="currentColor"/><path d="M17.6 12.4a4.6 4.6 0 014.6 4.6" stroke="currentColor" stroke-width="2.1" stroke-linecap="round" opacity=".5"/></svg></div>
  <div class="bar-txt">
    <h1>rom-watch</h1>
    <p class="sub" id="brandSub">系统包监测</p>
  </div>
  <div class="live" id="live" role="status" aria-live="polite"><span class="dot"></span><span id="liveTxt">连接中…</span></div>
</header>

<section class="card hero" id="hero" data-state="idle">
  <div>
    <span class="badge idle" id="badge">读取中…</span>
    <div class="ver" id="ver">—</div>
    <p class="sub" id="heroSub">正在读取设备状态…</p>
    <p class="hint" id="foundLine" hidden><span id="foundTxt"></span> <button class="gh sm" id="bSeen" type="button" title="只是把报警灯摁灭 —— 不改“水位线”，也不等于跳过这版；出更新的版本还会再亮">👀 我知道了（关灯）</button></p>    <div class="act">
      <button class="pri" id="bAck" type="button">我已刷 ✓</button>
      <button class="dan" id="bSkip" type="button">跳过这版</button>
      <button id="bCheck" type="button">立即检查</button>
      <button id="bDl" type="button">⬇ 下载到电脑</button>
    </div>
    <p class="hint" id="dlHint" hidden>⬇ 已请求下载 <b id="dlVer"></b> —— 电脑那边几秒内会用 Edge 取走。<button class="gh sm" id="bDlClear" type="button">已取走／取消</button></p>
    <p class="hint">📂 <a id="linkVer" href="#">打开当前版本的 OpenList 页</a> · <a id="linkDir" href="#">监测目录</a></p>
    <p class="hint">板载 FLASH 键：短按＝我已刷，长按 1.5 秒＝跳过，按住 3 秒＝请求下载</p>
  </div>
  <div class="ringwrap">
    <div class="ring" aria-hidden="true"><i id="ringArc" style="--p:0"></i><span id="ringTxt">—</span></div>
    <p class="ringcap" id="nextTxt">下次检查 —</p>
  </div>
</section>

<details class="card howto" open>
  <summary title="三条就够：什么时候变红、刷完点什么、怎么让它马上去下载">怎么用（3 步）<span class="jr"> · 点这行可以收起</span></summary>
  <ol class="steps">
    <li>有新版本时，上面这张卡会<b>变红并亮起 🔔</b>（板子上那颗 LED 也会常亮）。</li>
    <li>刷完点【我已刷 ✓】，提示就消失；这版不想刷就点【跳过这版】。</li>
    <li>想让电脑立刻去下载最新包：点【⬇ 下载到电脑】，或者直接在板子上<b>按住 FLASH 键 3 秒</b>。</li>
  </ol>
  <p class="hint">名词解释：<b>水位线</b>＝你已经确认过的版本（比它旧的都不会再提醒你）；<b>黑匣子</b>＝设备上次怎么开机/重启/卡住的记录，出事才看；<b>轮询间隔</b>＝隔多久去查一次有没有新包。</p>
</details>

<div class="grid">
  <section class="card wide">
    <h2>设备状态</h2>
    <div class="tiles">
      <div class="tile"><small>WiFi 信号</small><b id="tRssi">—</b><span class="bars" id="tBars"></span></div>
      <div class="tile"><small>可用堆内存</small><b id="tHeap">—</b></div>
      <div class="tile"><small>已运行</small><b id="tUp">—</b></div>
      <div class="tile"><small>上次成功检查</small><b id="tLast">—</b></div>
    </div>
    <dl class="kv">
      <dt>本机 IP</dt><dd id="kIp">—</dd>
      <dt>设备名</dt><dd id="kName">—</dd>
      <dt>WiFi</dt><dd id="kSsid">—</dd>
      <dt>芯片 / MAC</dt><dd id="kChip">—</dd>
      <dt>固件</dt><dd id="kFw">—</dd>
      <dt>串口屏</dt><dd id="kTjc">—</dd>
      <dt>数据源</dt><dd id="kPreset">—</dd>
      <dt title="板载那颗蓝色 LED：有未确认的新版本时应当常亮；摁过「我知道了」后熄灭">报警灯<span class="jr">（有新版本应当常亮）</span></dt><dd id="kLed">—</dd>
      <dt title="水位线 = 你已经确认过的版本；比它旧的包都不会再提醒你">水位线<span class="jr">（＝你已经确认过的版本）</span></dt><dd id="kAck">—</dd>
      <dt>已跳过</dt><dd id="kSkip">—</dd>
      <dt>轮询间隔</dt><dd id="kIv">—</dd>
      <dt>设备时间</dt><dd id="kNow">—</dd>
    </dl>
  </section>

  <section class="card">
    <h2>监测设置</h2>
    <form id="cfgForm" autocomplete="off">
      <label for="fPreset" title="换一种监测目标：网盘目录 / GitHub 仓库 / 任意 JSON 接口">数据源<span class="jr"> · 在盯什么</span></label>
      <select id="fPreset" name="preset">
        <option value="0">OpenList 目录（网盘：比对目录里的最新一项）</option>
        <option value="1">GitHub Release（比对仓库最新发行版）</option>
        <option value="2">自定义 JSON（自己填键名）</option>
      </select>
      <label for="fPath" title="OpenList：目录路径；GitHub：/repos/拥有者/仓库/releases/latest">路径<span class="jr"> · OpenList 是目录，GitHub 是 /repos/… </span></label>
      <input id="fPath" name="path" spellcheck="false">
      <label for="fHost" title="服务器域名或 IP，不用带 https://">服务器<span class="jr"> · 域名或 IP</span></label>
      <input id="fHost" name="host" spellcheck="false">
      <label for="fIv" title="隔多久去查一次；越小越灵敏，也越费流量">轮询间隔（秒，最小 60）<span class="jr"> · 多久查一次</span></label>
      <input id="fIv" name="interval" inputmode="numeric" spellcheck="false">
      <label for="fTitle" title="显示在网页/OLED/串口屏上的名字">标题<span class="jr"> · 显示的名字</span></label>
      <input id="fTitle" name="title" spellcheck="false">
      <details class="sub2">
        <summary title="一般不用改；换自定义数据源时才动">高级：取值/时间键 · 请求方式 · 网页基址</summary>
        <label for="fJsonKey" title="要在响应里找哪个字段当“版本号”">取值键<span class="jr"> · 例 name / tag_name</span></label>
        <input id="fJsonKey" name="jsonKey" spellcheck="false">
        <label for="fTimeKey" title="哪个字段代表“什么时候出的”，用它比大小选最新">时间键<span class="jr"> · 例 created / published_at</span></label>
        <input id="fTimeKey" name="timeKey" spellcheck="false">
        <label for="fMethod">请求方式</label>
        <select id="fMethod" name="method"><option value="1">POST（OpenList 用这个）</option><option value="0">GET（GitHub 用这个）</option></select>
        <label for="fWebBase" title="“打开当前版本”那个链接指向哪；留空＝按服务器+路径自动拼">网页基址（可留空）</label>
        <input id="fWebBase" name="webBase" spellcheck="false">
        <p class="hint">GitHub 例子：服务器 <span class="mono">api.github.com</span>、路径 <span class="mono">/repos/OWNER/REPO/releases/latest</span>、网页基址 <span class="mono">https://github.com/OWNER/REPO/releases</span>。
        周期别小于 10 分钟（GitHub 未登录限制 60 次/小时）。</p>
      </details>
      <div class="act two">
        <button class="pri" type="submit" id="bSave">保存设置</button>
        <button class="gh" type="button" id="bRevert">放弃修改</button>
      </div>
    </form>
  </section>

  <section class="card">
    <h2 title="黑匣子 = 设备上次怎么开机/重启/卡住的记录；平时不用管，出事（重启/掉线）才看这里">启动黑匣子<span class="jr"> · 上次是怎么关机的</span></h2>
    <div class="tiles">
      <div class="tile"><small>累计启动</small><b id="bBoots">—</b></div>
      <div class="tile"><small>上次崩在</small><b id="bPhase">—</b></div>
      <div class="tile"><small>上次连 WiFi</small><b id="bWifi">—</b></div>
      <div class="tile"><small>本次已到阶段</small><b id="bNow">—</b></div>
    </div>
    <dl class="kv">
      <dt title="复位 = 重启；这行说明上次它是怎么重启的（正常上电 / 看门狗 / 掉电 等）">上次复位原因<span class="jr">（＝上次怎么重启的）</span></dt><dd id="bReset">—</dd>
    </dl>
    <p class="hint">上次开机自检</p>
    <div class="chips" id="bSelf"></div>
    <p class="hint"><a href="/log">看完整日志（/log）→</a></p>
  </section>

  <section class="card wide">
    <h2>最近 6 次检测</h2>
    <div class="tw">
    <table>
      <thead><tr><th scope="col">时间</th><th scope="col">结果</th><th scope="col">抓到</th><th scope="col">来源</th></tr></thead>
      <tbody id="histBody"><tr><td colspan="4" class="sub">读取中…</td></tr></tbody>
    </table>
    </div>
  </section>

  <details class="card wide">
    <summary title="这里是从板子抓回来的原始数据，给排障用；看不懂可以直接忽略">调试：最近响应片段 / 完整状态文本<span class="jr"> · 排障用，看不懂可忽略</span></summary>
    <pre class="box mono" id="dbgBody">（还没有响应片段）</pre>
    <pre class="box mono" id="dbgState">—</pre>
  </details>

  <details class="card wide">
    <summary title="重启设备 / 重新配网这类操作，平时不用动，手滑会断网">维护与危险操作<span class="jr"> · 平时不用动</span></summary>
    <div class="act three">
      <button class="gh" type="button" id="bReboot">重启设备</button>
      <button class="gh" type="button" id="bRefresh2">重新读取状态</button>
      <button class="dan" type="button" id="bReconfig">重新配网（清空 WiFi 凭据）</button>
    </div>
    <p class="hint">重新配网会清掉 WiFi 配置并重启进入配网热点（<span class="mono">ROMWatch-&lt;芯片号&gt;</span>，地址 <span class="mono">http://192.168.4.1/</span>）—— 需要重新填密码。</p>
  </details>
</div>

<footer class="foot">
  <nav class="links" aria-label="快捷端点">
    <a href="/rom">/rom</a><a href="/log">/log</a><a href="/status">/status</a>
    <a href="/update">固件升级</a><a href="/i2c">I2C 扫描</a><a href="/tjcwire">屏线路体检</a>
  </nav>
  <div class="footctl">
    <button class="gh sm" type="button" id="bRefresh">立即刷新</button>
    <span class="zctl" title="大屏看不清就调大；地址栏里的 ?scale= 会跟着变，下次打开还是这个大小">
      <span>字号</span>
      <button class="gh sm" type="button" id="zDown" aria-label="缩小字号" title="缩小字号">−</button>
      <b id="zNow" class="mono" aria-live="polite">100%</b>
      <button class="gh sm" type="button" id="zUp" aria-label="放大字号" title="放大字号">＋</button>
    </span>
  </div>
  <span>板载 FLASH 键：短按＝我已刷，长按 1.5 秒＝跳过，按住 3 秒＝请求下载</span>
  <noscript><span>· 本页的自动刷新需要 JavaScript；也可以直接看 <a href="/log">/log</a> 文本。</span></noscript>
</footer>
<p class="kexit" id="kexit"><a id="kexitA" href="?kiosk=0" title="退出看板模式，回到完整页面">退出看板</a></p>
</div>
<div class="toasts" id="toasts" aria-live="polite"></div>
)HTMLBODY";

static const char UI_TAIL[] PROGMEM = R"HTMLJS(
<script>
(function(){
"use strict";
var $ = function(id){ return document.getElementById(id); };
var E = {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'};
function esc(s){ return String(s == null ? '' : s).replace(/[&<>"]/g, function(c){ return E[c]; }); }
function pad(n){ return (n < 10 ? '0' : '') + n; }
function mmss(s){ s = Math.max(0, s | 0); return Math.floor(s / 60) + ':' + pad(s % 60); }
function dur(s){ s = Math.max(0, s | 0); var h = Math.floor(s/3600), m = Math.floor(s%3600/60), x = s%60;
  if (h) return h + ' 小时 ' + m + ' 分';
  if (m) return m + ' 分 ' + x + ' 秒';
  return x + ' 秒'; }
function ago(s){ s = Math.max(0, s | 0);
  if (s < 60) return s + ' 秒前';
  if (s < 3600) return Math.floor(s/60) + ' 分钟前';
  if (s < 86400) return Math.floor(s/3600) + ' 小时前';
  return Math.floor(s/86400) + ' 天前'; }
function toast(msg, bad){
  var w = $('toasts'), d = document.createElement('div');
  d.className = 'toast' + (bad ? ' bad' : '');
  d.textContent = msg;
  w.appendChild(d);
  setTimeout(function(){ d.style.transition = 'opacity .3s'; d.style.opacity = '0';
    setTimeout(function(){ if (d.parentNode) d.parentNode.removeChild(d); }, 340); }, 2400);
}
function bars(rssi){
  var lvl = rssi >= -55 ? 5 : rssi >= -65 ? 4 : rssi >= -72 ? 3 : rssi >= -80 ? 2 : 1, o = '', i;
  for (i = 1; i <= 5; i++) o += '<i class="' + (i <= lvl ? 'on' : '') + '" style="height:' + (5 + i * 2) + 'px"></i>';
  return o;
}
function chip(ok, label){
  return '<span class="chip ' + (ok ? 'y' : 'n') + '">' + esc(label) + (ok ? ' OK' : ' ✗') + '</span>';
}
function setVal(el, v){ if (document.activeElement !== el && el.value !== String(v)) el.value = v; }

var S = { j: null, nextIn: 0, waitS: 600, sig: '', polls: 0, lastAt: 0 };

/* ★大屏/看板/字号（全部由 URL 参数驱动，纯前端生效，服务端一行都不用改）：
 *   ?scale=0.8~1.6（默认 1）→ documentElement.style.zoom（Edge/Chrome 有效）；
 *   ?kiosk=1 → <body class="kiosk">（CSS 里定义「只留状态卡＋4 个小块＋最近 6 次检测」）；
 *   页脚「字号」−/＋ 改完用 history.replaceState 写回 URL ⇒ 不整页刷新、不打断 5 秒轮询 ✓
 *   （首帧前已经由 <body> 后的早脚本设过一次，这里再做一遍是防它失败）*/
var PARAM = new URLSearchParams(location.search);
var SC = parseFloat(PARAM.get('scale'));
var KIOSK = PARAM.get('kiosk') === '1';
if (!(SC >= 0.8 && SC <= 1.6)) SC = 1;
function zTxt(){ return Math.round(SC * 100) + '%'; }
function zUrl(){
  var p = new URLSearchParams(location.search);
  if (SC === 1) p.delete('scale'); else p.set('scale', String(SC));
  if (KIOSK) p.set('kiosk', '1'); else p.delete('kiosk');
  var q = p.toString();
  history.replaceState(null, '', location.pathname + (q ? '?' + q : '') + location.hash);
}
function zApply(){
  document.documentElement.style.zoom = String(SC);
  if (KIOSK) document.body.className = 'kiosk';
  var now = $('zNow'); if (now) now.textContent = zTxt();
  zUrl();
  var ex = $('kexitA');
  if (ex){                       // 退出看板：带上当前字号，别把大屏上调好的大小弄丢 ✓
    var p = new URLSearchParams(location.search);
    p.set('kiosk', '0');
    ex.href = location.pathname + '?' + p.toString();
  }
}
function zStep(d){
  var n = Math.round((SC + d) * 10) / 10;
  if (n < 0.8 || n > 1.6){ toast('字号已经到头了（' + zTxt() + '）'); return; }
  SC = n; zApply(); toast('字号 ' + zTxt());
}
$('zUp').onclick = function(){ zStep(0.1); };
$('zDown').onclick = function(){ zStep(-0.1); };
zApply();

function heroSub(j){
  var t;
  if (!j.latest) t = '还没抓到目录数据 —— 等下一轮，或点【立即检查】';
  else if (j.hasNew) t = '上传时间 ' + (j.latestTime || '未知') + ' · 水位线 ' + (j.ack || '未确认');
  else t = '水位线 ' + (j.ack || '—') + ' · 上次成功 ' + ago(Math.max(0, j.up - j.lastOkAt));
  if (j.fails > 0) t += ' · 连续失败 ' + j.fails + (j.lastErr ? '（' + j.lastErr + '）' : '');
  return t;
}

function paint(){
  var j = S.j; if (!j) return;
  var iv = Math.max(60, S.waitS);
  var p = Math.max(0, Math.min(100, S.nextIn / iv * 100));
  $('ringArc').style.setProperty('--p', p.toFixed(1));
  $('ringTxt').textContent = mmss(S.nextIn);
  $('nextTxt').textContent = '下次检查 ' + mmss(S.nextIn);
  $('tUp').textContent = dur(j.up);
  $('tLast').textContent = j.lastOkAt ? ago(Math.max(0, j.up - j.lastOkAt)) : '从未';
  if (S.lastAt){
    var s = Math.round((Date.now() - S.lastAt) / 1000);
    $('liveTxt').textContent = '在线 · ' + (s < 2 ? '刚刚更新' : s + ' 秒前更新');
  }
}

function render(j){
  S.j = j;
  var bb = j.bb || {}, cur = j.cur || {}, hist = j.hist || [];   // ★缺字段也别让整页崩（版本回滚也不会白屏 ✓）
  var online = !!j.ip && j.ip !== '0.0.0.0';
  var st = !j.latest ? 'idle' : (j.hasNew ? 'new' : 'ok');
  if (!online) st = 'offline';
  var hero = $('hero'), badge = $('badge');
  hero.setAttribute('data-state', st);
  badge.className = 'badge' + (st === 'ok' ? '' : ' ' + st);
  badge.textContent = st === 'offline' ? '设备离线' : st === 'new' ? '🔔 有新包' :
                      st === 'idle' ? '⏳ 等待首次检查' : '✓ 已是最新';
  $('ver').textContent = j.latest || '—';
  $('heroSub').textContent = heroSub(j);
  $('kName').textContent = j.name || '—';

  var dir = 'https://' + j.host + j.path;
  $('linkDir').href = dir;
  $('linkVer').href = j.latest ? dir + '/' + j.latest : dir;

  S.waitS = Math.max(60, j.waitS || j.interval || 600);
  S.nextIn = Math.max(0, Math.min(S.waitS, j.nextIn | 0));

  $('tRssi').textContent = online ? (j.rssi + ' dBm') : '离线';
  $('tBars').innerHTML = online ? bars(j.rssi) : '';
  $('tHeap').textContent = (j.heap / 1024).toFixed(1) + ' KB';
  $('kIp').textContent = j.ip || '—';
  $('kSsid').textContent = j.ssid || '—';
  $('kChip').textContent = (j.chip || '—') + ' · ' + (j.mac || '—');
  $('kFw').textContent = j.ver + ' (' + j.build + ')';
  /* ★串口屏那行要分清"没编屏"与"编了但没应答"（审计提醒：=0 时报"无应答（探测 115200）"是假信息 ✗）*/
  if (j.tjcEn === false)      $('kTjc').textContent = '本固件未编串口屏（USE_TJC=0）';
  else if (j.tjcOk)           $('kTjc').textContent = '已应答 · ' + j.tjcBaud + ' · ' + j.tjcInfo;
  else                        $('kTjc').textContent = '无应答（探测 ' + j.tjcBaud + '）';
  $('kPreset').textContent = (j.presetName || '—') + ' · ' + (j.method === 1 ? 'POST' : 'GET') +
                             ' · ' + (j.jsonKey || '?') + '/' + (j.timeKey || '?');
  /* ★标题可配：网页标题、顶栏副标题都跟着设备走 ✓（没配就保持原样 ✓）*/
  var ttl = j.title || '';
  if (ttl) {
    document.title = 'rom-watch · ' + ttl;
    $('brandSub').textContent = ttl;
  }
  /* ★报警灯那行要能自己解释（2026-09-29 主人报"有新版本怎么不会亮灯"⇒ 灯的状态必须看得见 ✓）*/
  if (j.ledAlarm)                 $('kLed').textContent = '常亮（有新版本没看过）';
  else if (j.hasNew)              $('kLed').textContent = '灭 · 你已摁过「我知道了」';
  else if (j.led)                 $('kLed').textContent = '亮（手动测试中）';
  else                            $('kLed').textContent = '灭（正常）';
  /* ★"首次发现时刻"（历史表只留 6 条答不上来"什么时候抓到的" ⇒ 单独存了一个字段 ✓）*/
  var fTxt = '';
  if (j.hasNew && j.found) {
    var dur2 = '';
    if (j.foundUp && j.up >= j.foundUp) dur2 = '，已提醒 ' + dur(j.up - j.foundUp);
    fTxt = '🔔 这个版本首次发现于 ' + j.found + dur2;
  }
  $('foundTxt').textContent = fTxt;
  $('foundLine').hidden = !fTxt;
  $('bSeen').hidden = !(j.hasNew && j.ledAlarm);
  $('kAck').textContent = j.ack || '未确认';
  $('kSkip').textContent = j.skip || '—';
  $('kIv').textContent = j.interval + ' 秒';
  $('kNow').textContent = j.clock ? j.now : '未校时（开机计时）';

  var self = [['显示屏', bb.oled], ['总线', bb.i2c], ['按键', bb.btn], ['配置', bb.cfg], ['网络', cur.net]];
  $('bBoots').textContent = (bb.boots || 0) + ' 次';
  $('bPhase').textContent = bb.phaseLast ? (bb.phaseLastTxt || '—') : '刚升级过';
  $('bWifi').textContent = bb.wifiMs ? (bb.wifiMs + ' ms') : '没连上';
  $('bReset').textContent = bb.reset || '—';
  $('bNow').textContent = cur.phaseTxt || '—';
  var h = '', i;
  for (i = 0; i < self.length; i++) h += chip(self[i][1], self[i][0]);
  $('bSelf').innerHTML = h;

  setVal($('fPath'), j.path); setVal($('fHost'), j.host); setVal($('fIv'), j.interval);
  setVal($('fTitle'), ttl); setVal($('fJsonKey'), j.jsonKey); setVal($('fTimeKey'), j.timeKey);
  setVal($('fWebBase'), j.webBase);
  if (document.activeElement !== $('fPreset')) $('fPreset').value = String(j.preset == null ? 0 : j.preset);
  if (document.activeElement !== $('fMethod')) $('fMethod').value = String(j.method === 1 ? 1 : 0);
  $('bDl') .disabled = false;
  $('dlHint').hidden = !j.dl;
  if (j.dl) $('dlVer').textContent = j.dl;

  var sig = JSON.stringify(hist);
  if (sig !== S.sig){
    S.sig = sig;
    var rows = '';
    if (!hist.length) rows = '<tr><td colspan="4" class="sub">还没有记录 —— 等下一轮，或点【立即检查】</td></tr>';
    for (i = 0; i < hist.length; i++){
      var e = hist[i];
      /* ★"来源"这一列是 2026-09-29 加的：换过数据源之后，历史里会同时有 GitHub / OpenList 的记录，
       *   不标来源就会被看成"查错了" ✗（本喵那次验收就是这么误会自己的 ✓）*/
      /* ★时间列缩成 `MM-DD HH:MM`（完整时刻放 title 里，鼠标/长按能看到 ✓）——
       *   手机上 19 个字符的完整时刻会把右边两列挤到换行 ✗（见上面 td 的注释 ✓）*/
      var tFull = esc(e.txt);
      var tShow = tFull.length >= 16 ? tFull.slice(5, 16) : tFull;
      rows += '<tr><td title="' + tFull + '">' + tShow + '</td><td class="' + (e.ok ? 'ok' : 'bad') + '">' +
              (e.ok ? '成功' : '失败 ×' + e.fails) + '</td><td>' + (esc(e.ver) || '—') + '</td><td>' +
              esc(e.srcName || '—') + '</td></tr>';
    }
    $('histBody').innerHTML = rows;
  }
  $('dbgBody').textContent = j.lastBody || '（还没有响应片段）';
  $('dbgState').textContent =
    '目标     : ' + j.host + j.path + '\n' +
    '最新包   : ' + (j.latest || '(还没抓到)') + (j.latestTime ? '   上传 ' + j.latestTime : '') + '\n' +
    '水位线   : ' + (j.ack || '(未确认)') + (j.skip ? '   已跳过 ' + j.skip : '') + '\n' +
    '间隔     : ' + j.interval + ' 秒\n' +
    '设备时间 : ' + (j.clock ? j.now : '未校时') + '\n' +
    '上次成功 : ' + (j.lastOkAt ? ago(j.up - j.lastOkAt) : '从未') + '\n' +
    '本机     : ' + j.ip + '  rssi ' + j.rssi + ' dBm  heap ' + j.heap + ' B\n' +
    '固件     : ' + j.name + ' ' + j.ver + ' build ' + j.build + '\n' +
    '串口屏   : ' + (j.tjcOk ? '已应答 ' + j.tjcInfo : '无应答') + '  (波特率 ' + j.tjcBaud + ')\n' +
    '复位     : ' + j.bb.reset + '  启动 ' + j.bb.boots + ' 次  上次阶段 ' + j.bb.phaseLastTxt;
  paint();
}

function live(ok){
  $('live').className = 'live ' + (ok ? 'on' : 'off');
  if (!ok) $('liveTxt').textContent = '连接中断，重试中…';
}

function poll(quiet){
  return fetch('/rom', {cache: 'no-store'}).then(function(r){
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).then(function(j){
    S.lastAt = Date.now(); S.polls++;
    live(true); render(j);
  }).catch(function(e){
    live(false);
    if (!quiet) toast('读取状态失败：' + e.message, true);
  });
}

function act(btn, url, msg, confirmText){
  if (confirmText && !window.confirm(confirmText)) return;
  var old = btn.textContent;
  btn.disabled = true; btn.textContent = '处理中…';
  fetch(url, {method: 'POST', redirect: 'manual'}).catch(function(){
    return fetch(url, {method: 'POST', mode: 'no-cors'}).catch(function(){});
  }).then(function(){
    toast(msg);
    setTimeout(poll, 500); setTimeout(poll, 2600);
  }).then(function(){
    btn.disabled = false; btn.textContent = old;
  });
}

$('bAck').onclick    = function(e){ act(e.currentTarget, '/ack',    '已确认水位线 ✓'); };
$('bSkip').onclick   = function(e){ act(e.currentTarget, '/skip',   '已跳过这一版'); };
$('bCheck').onclick  = function(e){ act(e.currentTarget, '/check',  '已请求立即检查，几秒后刷新 ✓'); };
$('bDl').onclick     = function(e){ act(e.currentTarget, '/dl',     '已请求下载 —— 电脑端几秒内会用 Edge 取走 ✓'); };
$('bDlClear').onclick= function(e){ act(e.currentTarget, '/dlclear','已清除下载请求'); };
$('bSeen').onclick   = function(e){ act(e.currentTarget, '/seen',   '好，灯灭了 ✓（页面照旧显示有新版本；出更新的版本还会再亮）'); };
$('bReboot').onclick = function(e){ act(e.currentTarget, '/reboot', '设备重启中…', '确定重启设备吗？'); };
$('bReconfig').onclick = function(e){ act(e.currentTarget, '/reconfig', '已清除 WiFi 凭据，设备进入配网模式',
                                          '确定清除配网信息并重新配网吗？设备会重启进入配网热点。'); };
$('bRefresh').onclick  = function(){ poll(false); toast('已刷新'); };
$('bRefresh2').onclick = function(){ poll(false); toast('已刷新'); };
$('bRevert').onclick   = function(){ poll(false); toast('已恢复为设备上的设置'); };

/* ★预设套用（2026-09-29 加）：光是"选预设"是不够的 ✗ —— 表单会把当前键名一起提交上去，
 *   那样换了数据源、键名还是旧的 ⇒ 抓不到东西 ✗。所以这里在**选项一变就把键名填好**，
 *   用户再点保存，提交的就是正确组合 ✓（去掉这一步的话，换预设等于白换 ✓）*/
var PRESETS = {
  '0': { jsonKey: 'name',     timeKey: 'created',      method: '1', tip: 'OpenList：服务器填域名、路径填目录 ✓' },
  '1': { jsonKey: 'tag_name', timeKey: 'published_at', method: '0',
         tip: 'GitHub：服务器填 api.github.com、路径填 /repos/OWNER/REPO/releases/latest ✓（间隔别小于 10 分钟）' },
  '2': null
};
$('fPreset').onchange = function(){
  var p = PRESETS[this.value];
  if (!p) { toast('自定义：下面"取值键/时间键"请自己填 ✓'); return; }
  $('fJsonKey').value = p.jsonKey;
  $('fTimeKey').value = p.timeKey;
  $('fMethod').value  = p.method;
  toast(p.tip + ' —— 记得点【保存设置】');
};

$('cfgForm').onsubmit = function(ev){
  ev.preventDefault();
  var q = new URLSearchParams(new FormData(ev.target)).toString();
  var btn = $('bSave');
  btn.disabled = true;
  fetch('/cfg', {method: 'POST', redirect: 'manual', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: q})
    .catch(function(){ return fetch('/cfg', {method: 'POST', mode: 'no-cors', body: new FormData(ev.target)}).catch(function(){}); })
    .then(function(){ toast('设置已保存 ✓'); setTimeout(poll, 700); setTimeout(poll, 3000); })
    .then(function(){ btn.disabled = false; });
};

var t = 0;
setInterval(function(){
  if (document.hidden) return;
  if (t % 5 === 0) poll(true);
  t++;
  if (S.j){ S.j.up++; if (S.nextIn > 0) S.nextIn--; paint(); }
}, 1000);
document.addEventListener('visibilitychange', function(){ if (!document.hidden) poll(true); });
poll(false);
})();
</script>
</body></html>
)HTMLJS";
