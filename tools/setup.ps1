<#
setup.ps1 —— rom-watch 一键「把编译环境准备好」
================================================================================
给新手用：不用懂 arduino-cli，一条命令把工具链 + core + 库全部搞定，
最后顺手编译一遍证明「环境真的能用」。

用法示例（在仓库根目录 serial-screen\ 下执行）：

    # 最常用：一条命令搞定一切
    pwsh -File tools\setup.ps1

    # 老版 Windows PowerShell 也行（5.1 不崩）
    powershell -File tools\setup.ps1

    # 只看用法，不动任何东西
    pwsh -File tools\setup.ps1 -Help

    # 编译别的固件工程（firmware\ota-base）
    pwsh -File tools\setup.ps1 -Sketch ota-base

    # 换板型（默认 esp8266:esp8266:d1_mini）
    pwsh -File tools\setup.ps1 -Fqbn esp8266:esp8266:nodemcuv2
    # 也可以走环境变量（和 tools\build_matrix.py / tools\ota.py 一致）
    $env:ROMWATCH_FQBN = 'esp8266:esp8266:nodemcuv2'

    # 只装环境、先不编译（网络慢、或者还没填 config.h 时）
    pwsh -File tools\setup.ps1 -SkipCompile

参数：
    -Sketch <名字>   固件工程名，默认 rom-watch（目录 firmware\<名字>）
    -Fqbn   <fqbn>   板型，默认 esp8266:esp8266:d1_mini；环境变量 ROMWATCH_FQBN 次之
    -SkipCompile     跳过最后那一步编译
    -Help            只打印用法

它做了什么（5 步，每步都会打印人话进度）：
    1. 找 arduino-cli（先看 tools\arduino-cli\，再看 PATH；都没有就告诉你去哪装，不偷偷下载）
    2. 检查 / 安装 ESP8266 core（esp8266:esp8266，本仓库实测 3.1.2；已装就跳过）
    3. 检查 / 安装 OLED 库（ESP8266 and ESP32 OLED driver for SSD1306 displays；已装就跳过）
    4. 检查 firmware\<名字>\config.h 在不在；不在就明确告诉你下一步怎么办
    5. 跑一次编译（--fqbn），打印 RAM / IRAM / Flash 占用

★本脚本不碰你的板子、不碰串口 —— 只装环境 + 编译。
  要烧到板子上请用 tools\flash.ps1（那一步才需要插板子）。

★说明：脚本会把 arduino-cli 的数据目录/库目录固定在仓库内的 tools\ 下
  （和 tools\build_matrix.py 的做法一致，见其 ARDUINO_DIRECTORIES_* 约定）：
      tools\arduino15   ← core / 工具链（ARDUINO_DIRECTORIES_DATA）
      tools\Arduino     ← 第三方库      （ARDUINO_DIRECTORIES_USER）
  这样不会污染你系统里的 Arduino 环境，删掉 tools\ 就等于卸载干净。
#>

[CmdletBinding()]
param(
    [string] $Sketch = 'rom-watch',
    [string] $Fqbn,
    [switch] $SkipCompile,
    [switch] $Help
)

# 任何未捕获的报错都变成「人话 + 下一步」，而不是一堆红字堆栈
$ErrorActionPreference = 'Stop'

# ----------------------------------------------------------------- 仓库路径
# 全部用 $PSScriptRoot / Join-Path 拼，绝对路径里不写任何个人目录
$ToolsDir = $PSScriptRoot
$Root     = Split-Path -Parent $ToolsDir
$CliLocal = Join-Path $ToolsDir 'arduino-cli\arduino-cli.exe'
$DataDir  = Join-Path $ToolsDir 'arduino15'
$UserDir  = Join-Path $ToolsDir 'Arduino'
$CfgFile  = Join-Path $DataDir  'arduino-cli.yaml'
$SketchDir = Join-Path (Join-Path $Root 'firmware') $Sketch
$SketchName = $Sketch
$BuildDir = Join-Path (Join-Path (Join-Path $Root 'firmware') 'build') $Sketch
$InoPath  = Join-Path $SketchDir ($Sketch + '.ino')
$CfgPath  = Join-Path $SketchDir 'config.h'
$CfgExample = Join-Path $SketchDir 'config.example.h'
$CliPath  = $null      # 第 1 步填

# 板型：参数 > 环境变量 ROMWATCH_FQBN > 默认
if ([string]::IsNullOrWhiteSpace($Fqbn)) { $Fqbn = $env:ROMWATCH_FQBN }
if ([string]::IsNullOrWhiteSpace($Fqbn)) { $Fqbn = 'esp8266:esp8266:d1_mini' }

# core / 库的名字（和 README「三分钟上手」里那两条命令一致）
$CoreId   = 'esp8266:esp8266'
$CoreWant = '3.1.2'                                          # 本仓库实测版本
$LibName  = 'ESP8266 and ESP32 OLED driver for SSD1306 displays'
$CliDocUrl = 'https://arduino.github.io/arduino-cli/latest/installation/'

# ----------------------------------------------------------------- 人话输出小工具
function Say([string]$m = '') { Write-Host $m }
function Step([string]$m) { Write-Host ''; Write-Host ("=== " + $m + " ===") -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host ("  [OK] " + $m) -ForegroundColor Green }
function Warn([string]$m) { Write-Host ("  [!] " + $m) -ForegroundColor Yellow }
function Info([string]$m) { Write-Host ("      " + $m) }

# 失败统一出口：说清楚「下一步该怎么办」，然后退出
function Fail([string]$what, [string[]]$next) {
    Write-Host ''
    Write-Host ("[X] " + $what) -ForegroundColor Red
    foreach ($n in $next) { Write-Host ("    -> " + $n) -ForegroundColor Yellow }
    Write-Host ''
    exit 1
}

# 用法
function Show-Usage {
    Say ''
    Say 'rom-watch 环境准备（setup.ps1）—— 一条命令把编译环境装好'
    Say '--------------------------------------------------------------'
    Say '  pwsh -File tools\setup.ps1                  最常用：装环境 + 编译一遍'
    Say '  pwsh -File tools\setup.ps1 -SkipCompile     只装环境，先不编译'
    Say '  pwsh -File tools\setup.ps1 -Sketch ota-base 换固件工程'
    Say '  pwsh -File tools\setup.ps1 -Fqbn esp8266:esp8266:nodemcuv2   换板型'
    Say '  pwsh -File tools\setup.ps1 -Help            看这份用法'
    Say ''
    Say '  环境变量：ROMWATCH_FQBN 也能指定板型（和 build_matrix.py / ota.py 一致）'
    Say ''
    Say '它只装环境 + 编译，不碰板子；要烧录请用 tools\flash.ps1'
    Say ''
}

# 跑外部命令，返回 @{ Code; Out }；stderr 单独收，避免 PS 5.1 的 2>&1 把错误当异常
function Invoke-Cli([string[]]$CliArgs) {
    $out = ''
    $err = ''
    $code = 1
    try {
        $out = (& $CliPath @CliArgs 2>$null | Out-String)
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    } catch {
        $err = $_.Exception.Message
        $code = 1
    }
    return [pscustomobject]@{ Code = [int]$code; Out = (([string]$out) + [string]$err) }
}

if ($Help) { Show-Usage; exit 0 }

# ----------------------------------------------------------------- 开场
Say ''
Say 'rom-watch 环境准备 —— 一共 5 步，跟着走就行'
Info ('仓库根目录 : ' + $Root)
Info ('固件工程   : ' + $SketchDir)
Info ('板型 FQBN  : ' + $Fqbn + '   （可用 -Fqbn 或环境变量 ROMWATCH_FQBN 覆盖）')
Info ('core/库目录: ' + $DataDir + '  |  ' + $UserDir)

# ================================================================= 第 1 步：找 arduino-cli
Step '第 1 步 / 共 5 步：找 arduino-cli'

if (Test-Path $CliLocal) {
    $CliPath = $CliLocal
    Ok ('用仓库自带的：' + $CliPath)
} else {
    Warn ('仓库里没有 tools\arduino-cli\arduino-cli.exe（' + $CliLocal + '）')
    $onPath = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($onPath) {
        $CliPath = $onPath.Source
        Ok ('改用 PATH 里的：' + $CliPath)
    }
}
if (-not $CliPath) {
    Fail '找不到 arduino-cli，装不了环境。' @(
        ('去官网下载（Windows 选 arduino-cli_x.y.z_Windows_64bit.zip）：' + $CliDocUrl),
        '解压出 arduino-cli.exe，放到 tools\arduino-cli\arduino-cli.exe（本仓库会自动认它）',
        '或者把它所在目录加进 PATH，然后重新跑本脚本',
        '★本脚本故意不替你下载：免得你机器上多出一份不知道哪来的 exe'
    )
}

$ver = Invoke-Cli @('version')
if ($ver.Code -eq 0) {
    $firstLine = (([string]$ver.Out) -split "`n")[0].Trim()
    Ok ('arduino-cli 可用：' + $firstLine)
} else {
    Warn 'arduino-cli 能执行但 version 报错，继续试着往下走；不行就重新下载一份'
}

# 固定数据目录 / 库目录（不污染系统环境，和 build_matrix.py 的做法一致）
$env:ARDUINO_DIRECTORIES_DATA = $DataDir
$env:ARDUINO_DIRECTORIES_USER = $UserDir
Info ('已设置 ARDUINO_DIRECTORIES_DATA = ' + $DataDir)
Info ('已设置 ARDUINO_DIRECTORIES_USER = ' + $UserDir)

# 固定参数：都走仓库内的配置，保证「编译用哪个 core、哪个库」是确定的
$CliBase = @()
if (Test-Path $CfgFile) { $CliBase += @('--config-file', $CfgFile) }

# ================================================================= 第 2 步：core
Step ('第 2 步 / 共 5 步：ESP8266 core（' + $CoreId + '，本仓库实测 ' + $CoreWant + '）')

$hasCore = $false
$coreList = Invoke-Cli ($CliBase + @('core', 'list'))
if ($coreList.Code -eq 0 -and ([string]$coreList.Out) -match [regex]::Escape($CoreId)) {
    $hasCore = $true
    $coreLine = (([string]$coreList.Out) -split "`n" | Where-Object { $_ -match [regex]::Escape($CoreId) } | Select-Object -First 1)
    Ok ('已经装过了，跳过：' + $coreLine.Trim())
    if (([string]$coreList.Out) -notmatch [regex]::Escape($CoreWant)) {
        Warn ('注意：装的不是本仓库实测的 ' + $CoreWant + '；编不过时可以跑：')
        Info ($CliPath + ' core install ' + $CoreId + '@' + $CoreWant)
    }
}

if (-not $hasCore) {
    Info '没装过，现在装（第一次要下载几百 MB，慢是正常的，别关窗口）…'
    $install = Invoke-Cli ($CliBase + @('core', 'install', $CoreId))
    if ($install.Code -ne 0) {
        Fail ('装 core 失败（' + $CoreId + '）。' ) @(
            '① 先看上面输出里有没有 timeout / proxy / connection 字样 —— 多半是网络问题，重跑一次即可',
            ('② 如果你机器上 tools\arduino15\arduino-cli.yaml 里 network.proxy 指的代理已经不用了，把那行删掉或改成你自己的代理再试'),
            ('③ 想固定版本：' + $CliPath + ' core install ' + $CoreId + '@' + $CoreWant),
            ('④ 手动下载包索引：' + $CliPath + ' core update-index')
        )
    }
    Ok ('core 装好了：' + $CoreId)
}

# ================================================================= 第 3 步：库
Step '第 3 步 / 共 5 步：OLED 库（SSD1306 / SSD1315）'

$hasLib = $false
$libList = Invoke-Cli ($CliBase + @('lib', 'list'))
if ($libList.Code -eq 0 -and ([string]$libList.Out) -match 'OLED driver for SSD1306') {
    $hasLib = $true
    $libLine = (([string]$libList.Out) -split "`n" | Where-Object { $_ -match 'OLED driver for SSD1306' } | Select-Object -First 1)
    Ok ('已经装过了，跳过：' + $libLine.Trim())
}

if (-not $hasLib) {
    Info ('没装过，现在装：' + $LibName)
    $lib = Invoke-Cli ($CliBase + @('lib', 'install', $LibName))
    if ($lib.Code -ne 0) {
        Fail '装 OLED 库失败。' @(
            '① 先跑一次 lib update-index 刷新库索引，再重跑本脚本：',
            ('     ' + $CliPath + ' lib update-index'),
            '② 或者用库的编号装（稳定，不受名字空格影响）：',
            ('     ' + $CliPath + ' lib install 2972'),
            '③ 网络问题（timeout / connection）就重跑一次'
        )
    }
    Ok ('库装好了：' + $LibName)
}

# ================================================================= 第 4 步：config.h
Step '第 4 步 / 共 5 步：检查 config.h（你的私有配置）'

$configReady = $false
if (Test-Path $CfgPath) {
    $configReady = $true
    Ok ('找到了：' + $CfgPath)
    Info '（这个文件是你的私有配置：WiFi 密码 / 服务器地址，它不会进仓库，别手滑提交）'
} else {
    Warn ('还没有 ' + $CfgPath)
    Say ''
    Say '  ★ 下一步（二选一）：' -ForegroundColor Yellow
    Say ('     A) 复制样板再自己填：' + 'copy ' + $CfgExample + ' ' + $CfgPath)
    Say ('        然后编辑 config.h：填 WiFi；或者把 PVT_WIFI_SET 留 0（开机走 AP 配网，不用填密码）')
    Say ('        别忘了改掉默认 OTA 口令：加一行 #define OTA_PASSWORD "换成你自己的"')
    Say ('     B) 跑配置小工具生成：python tools\make_config.py')
    Say ''
    Info '★不填也能编译（固件有中性默认值 + 开机 AP 配网），但烧进去之后要手动配网。'
    Info '★本脚本继续往下走，先把编译环境验证完。'
}

# ================================================================= 第 5 步：编译
Step '第 5 步 / 共 5 步：编译验证'

if ($SkipCompile) {
    Warn '你用了 -SkipCompile，跳过编译。'
    Info ('想单独编一次：' + $CliPath + ' compile --fqbn ' + $Fqbn + ' ' + $SketchDir)
    Say ''
    Say '环境准备好了。下一步：pwsh -File tools\flash.ps1   （把板子插上 USB 再跑）'
    exit 0
}

if (-not (Test-Path $InoPath)) {
    Fail ('没找到固件主文件：' + $InoPath) @(
        '确认 -Sketch 的名字对不对（目录名 = 工程名 = .ino 文件名）',
        '看看 firmware\ 下面有哪些工程：Get-ChildItem firmware'
    )
}

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }
Info ('产物目录：' + $BuildDir)
Info ('编译命令：' + $CliPath + ' compile --fqbn ' + $Fqbn + ' --output-dir ' + $BuildDir + ' ' + $SketchDir)

$t0 = Get-Date
$comp = Invoke-Cli ($CliBase + @('compile', '--fqbn', $Fqbn, '--output-dir', $BuildDir, $SketchDir))
$secs = [int]((Get-Date) - $t0).TotalSeconds
$text = [string]$comp.Out

if ($comp.Code -ne 0) {
    Say ''
    Say '编译输出里的关键几行：' -ForegroundColor Yellow
    $shown = 0
    foreach ($line in ($text -split "`n")) {
        if ($shown -ge 20) { break }
        if ($line -match '(?i)error|warning:|fatal|not found|undefined') {
            Info $line.TrimEnd()
            $shown = $shown + 1
        }
    }
    if ($shown -eq 0) {
        Info '（没抓到 error 行，把上面完整输出往上翻一下）'
    }
    Fail ('编译失败（' + $Fqbn + '，耗时 ' + $secs + 's）。') @(
        '① 报 "not found / No such file" ⇒ 多半是库或 core 没装好，重跑本脚本一次',
        '② 报 IRAM 超了 ⇒ 关掉用不到的功能再编：加 --build-property "compiler.cpp.extra_flags=-DUSE_TJC=0"',
        '   （或者直接跑 python tools\build_matrix.py，看六种配置的实测占用）',
        '③ 报 config.h 里的语法错 ⇒ 打开 config.h 对着 config.example.h 检查引号有没有配对',
        '④ 编译产物目录被占用 ⇒ 关掉占用它的窗口（编辑器/杀毒）再重跑'
    )
}

Ok ('编译通过！耗时 ' + $secs + 's')

# 打印资源占用（arduino-cli 会打 used NNNN/NNNN bytes (NN%)，顺序固定 RAM / IRAM / Flash）
$rows = [regex]::Matches($text, 'used\s+(\d+)\s*/\s*(\d+)\s*bytes\s*\((\d+)%\)')
if ($rows.Count -ge 3) {
    $labels = @('RAM  ', 'IRAM ', 'Flash')
    for ($i = 0; $i -lt 3; $i++) {
        $used = $rows[$i].Groups[1].Value
        $total = $rows[$i].Groups[2].Value
        $pct = $rows[$i].Groups[3].Value
        Info ($labels[$i] + ' : ' + $used + ' / ' + $total + ' bytes  (' + $pct + '%)')
    }
    Info '★IRAM 是这块板子的命门（上限 65536）：超过就编不过，把用不到的功能关掉'
} else {
    Info '（没解析到资源占用行，不影响结果）'
}

$bin = Join-Path $BuildDir ($Sketch + '.ino.bin')
if (Test-Path $bin) {
    $kb = [math]::Round((Get-Item $bin).Length / 1024, 1)
    Ok ('固件产物：' + $bin + '  (' + $kb + ' KB)')
} else {
    Warn ('产物目录里没看到 ' + $Sketch + '.ino.bin —— 往上翻一下编译输出')
}

# ----------------------------------------------------------------- 收尾
if (-not $configReady) {
    Say ''
    Warn '提醒：config.h 还没建，烧进去之后要开机 AP 配网（手机连热点 → 打开 192.168.4.1 填 WiFi）。'
    Warn '想省这一步就现在补：copy ' + $CfgExample + ' ' + $CfgPath
}

Say ''
Say '环境就绪！' -ForegroundColor Green
Say '下一步：'
Say '  1) 把板子插上 USB'
Say '  2) pwsh -File tools\flash.ps1        （编译 + 烧录，串口自动探测）'
Say '  3) 烧完看串口日志里打印的 IP，用浏览器打开它'
Say '  （不想插线：pwsh -File tools\flash.ps1 -Ota  走无线 OTA）'
Say ''
exit 0
