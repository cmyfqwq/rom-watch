<#
flash.ps1 —— rom-watch 一键「编译 + 烧录到板子」
================================================================================
给新手用：一条命令把固件刷进板子，串口自动探测，不用手敲 arduino-cli。

用法示例（在仓库根目录 serial-screen\ 下执行）：

    # 最常用：插上板子，一条命令搞定
    pwsh -File tools\flash.ps1

    # 老版 Windows PowerShell 也行（5.1 不崩）
    powershell -File tools\flash.ps1

    # 只看用法，不动任何东西
    pwsh -File tools\flash.ps1 -Help

    # 电脑上有好几个串口，脚本列出来之后自己指定
    pwsh -File tools\flash.ps1 -Port COM5

    # 不插线，走无线 OTA（转调 tools\ota.py push：编译 + 推送 + 校验）
    pwsh -File tools\flash.ps1 -Ota

    # 刷完顺便盯着串口日志看（找板子 IP）
    pwsh -File tools\flash.ps1 -Monitor

    # 换板型 / 换固件工程
    pwsh -File tools\flash.ps1 -Fqbn esp8266:esp8266:nodemcuv2
    pwsh -File tools\flash.ps1 -Sketch ota-base

参数：
    -Sketch <名字>   固件工程名，默认 rom-watch（目录 firmware\<名字>）
    -Port   <COMx>   串口；不给就自动探测（优先 CH340 / CP210x / FT232 这类 USB 串口）
    -Fqbn   <fqbn>   板型，默认 esp8266:esp8266:d1_mini；环境变量 ROMWATCH_FQBN 次之
    -Ota             走无线 OTA：转调 python tools\ota.py push（不插线，板子要已在网上）
    -Monitor         烧完打开串口监视器（Ctrl-C 退出）
    -Help            只打印用法

它做了什么：
    1. 找 arduino-cli（先 tools\arduino-cli\，再 PATH；都没有就告诉你去哪装）
    2. 检查 config.h 在不在（不在就提示 copy config.example.h config.h 并填 WiFi）
    3. 编译到 firmware\build\<名字>\（和 tools\ota.py 的产物目录一致）
    4. 探测串口 → 上传（-Ota 时改为 tools\ota.py push）
    5. 打印「下一步」：浏览器打开板子的 IP（IP 会打印在串口日志里）

★哪一步碰你的板子：只有第 4 步（上传）会碰。跑之前请把板子插上 USB，
  Windows 里出现 COM 口（设备管理器 → 端口）。第 1~3 步纯本机操作，不碰板子。
  走 -Ota 时不碰 USB，但板子必须已经联上网、和电脑在同一网段。

★失败都会告诉你「下一步该怎么办」，不是甩一堆原始报错。
#>

[CmdletBinding()]
param(
    [string] $Sketch = 'rom-watch',
    [string] $Port,
    [string] $Fqbn,
    [switch] $Ota,
    [switch] $Monitor,
    [switch] $Help
)

$ErrorActionPreference = 'Stop'

# ----------------------------------------------------------------- 仓库路径
$ToolsDir  = $PSScriptRoot
$Root      = Split-Path -Parent $ToolsDir
$CliLocal  = Join-Path $ToolsDir 'arduino-cli\arduino-cli.exe'
$DataDir   = Join-Path $ToolsDir 'arduino15'
$UserDir   = Join-Path $ToolsDir 'Arduino'
$CfgFile   = Join-Path $DataDir  'arduino-cli.yaml'
$SketchDir = Join-Path (Join-Path $Root 'firmware') $Sketch
$SketchName = $Sketch
$BuildDir  = Join-Path (Join-Path (Join-Path $Root 'firmware') 'build') $Sketch
$InoPath   = Join-Path $SketchDir ($Sketch + '.ino')
$CfgPath   = Join-Path $SketchDir 'config.h'
$CfgExample = Join-Path $SketchDir 'config.example.h'
$OtaPy     = Join-Path $ToolsDir 'ota.py'
$CliPath   = $null      # 第 1 步填

$Baud      = 115200                                   # 和 tools\ota.py 的 BAUD 一致
$CliDocUrl = 'https://arduino.github.io/arduino-cli/latest/installation/'

if ([string]::IsNullOrWhiteSpace($Fqbn)) { $Fqbn = $env:ROMWATCH_FQBN }
if ([string]::IsNullOrWhiteSpace($Fqbn)) { $Fqbn = 'esp8266:esp8266:d1_mini' }
if ([string]::IsNullOrWhiteSpace($Port)) { $Port = $env:ROMWATCH_PORT }   # 和 ota.py 的环境变量一致

# ----------------------------------------------------------------- 人话输出小工具
function Say([string]$m = '') { Write-Host $m }
function Step([string]$m) { Write-Host ''; Write-Host ("=== " + $m + " ===") -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host ("  [OK] " + $m) -ForegroundColor Green }
function Warn([string]$m) { Write-Host ("  [!] " + $m) -ForegroundColor Yellow }
function Info([string]$m) { Write-Host ("      " + $m) }

function Fail([string]$what, [string[]]$next) {
    Write-Host ''
    Write-Host ("[X] " + $what) -ForegroundColor Red
    foreach ($n in $next) { Write-Host ("    -> " + $n) -ForegroundColor Yellow }
    Write-Host ''
    exit 1
}

function Show-Usage {
    Say ''
    Say 'rom-watch 烧录（flash.ps1）—— 编译 + 烧到板子，一条命令'
    Say '--------------------------------------------------------------'
    Say '  pwsh -File tools\flash.ps1                  插上板子，自动找串口并烧录'
    Say '  pwsh -File tools\flash.ps1 -Port COM5       有多个串口时自己指定'
    Say '  pwsh -File tools\flash.ps1 -Ota             不插线，走无线 OTA（转调 ota.py push）'
    Say '  pwsh -File tools\flash.ps1 -Monitor         烧完看串口日志（找板子 IP）'
    Say '  pwsh -File tools\flash.ps1 -Sketch ota-base 换固件工程'
    Say '  pwsh -File tools\flash.ps1 -Fqbn esp8266:esp8266:nodemcuv2   换板型'
    Say '  pwsh -File tools\flash.ps1 -Help            看这份用法'
    Say ''
    Say '  环境变量：ROMWATCH_FQBN（板型）、ROMWATCH_PORT（串口）'
    Say ''
    Say '环境还没装？先跑：pwsh -File tools\setup.ps1'
    Say ''
}

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

# ----------------------------------------------------------------- 串口探测
# 返回该串口的一条「人话描述」；拿不到描述就返回空串
function Get-PortInfo([string]$name) {
    $desc = ''
    try {
        $hit = Get-CimInstance -ClassName Win32_SerialPort -ErrorAction Stop |
               Where-Object { $_.DeviceID -eq $name } | Select-Object -First 1
        if ($hit) { $desc = [string]$hit.Description }
    } catch {
        $desc = ''
    }
    if ([string]::IsNullOrWhiteSpace($desc)) {
        # Win32_SerialPort 看不到时（少数驱动）退到 PnP 实体，按 (COMx) 抓
        try {
            $esc = [regex]::Escape('(' + $name + ')')
            $pnp = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
                   Where-Object { $_.Name -match $esc } | Select-Object -First 1
            if ($pnp) { $desc = [string]$pnp.Name }
        } catch {
            $desc = ''
        }
    }
    if ([string]::IsNullOrWhiteSpace($desc)) { $desc = '(没有描述)' }
    return $desc
}

# 按描述打分：USB 转串口芯片 > 泛 USB 串口 > 其他
function Get-PortScore([string]$desc) {
    $d = ([string]$desc).ToUpperInvariant()
    if ($d -match 'CH340|CH341|CP210|FT232|FTDI|SILICON LABS|PROLIFIC|PL2303') { return 3 }
    if ($d -match 'USB.*(SERIAL|UART|COM)|(SERIAL|UART).*USB') { return 2 }
    if ($d -match 'ESP|ESP8266|WEMOS') { return 3 }
    return 1
}

function Get-SerialPorts {
    $names = @()
    try {
        $names = @([System.IO.Ports.SerialPort]::GetPortNames())
    } catch {
        $names = @()
    }
    if ($names.Count -eq 0) {
        try {
            $names = @(Get-CimInstance -ClassName Win32_SerialPort -ErrorAction Stop |
                       ForEach-Object { $_.DeviceID })
        } catch {
            $names = @()
        }
    }
    $list = @()
    foreach ($n in ($names | Sort-Object -Unique)) {
        if ([string]::IsNullOrWhiteSpace($n)) { continue }
        $desc = Get-PortInfo $n
        $list += [pscustomobject]@{
            Name  = [string]$n
            Desc  = [string]$desc
            Score = [int](Get-PortScore $desc)
        }
    }
    # 分数高的排前面
    return @($list | Sort-Object -Property @{Expression = 'Score'; Descending = $true}, @{Expression = 'Name'; Descending = $false})
}

# 探到串口之后决定用哪个：正好一个 / 有明确的 USB 串口 / 让用户自己指定
function Resolve-Port {
    $ports = @(Get-SerialPorts)

    if ($ports.Count -eq 0) {
        Fail '没探测到任何串口（COM 口）。' @(
            '① 板子插好 USB 了吗？换一根线试试（有些线只能充电、不能传数据）',
            '② 打开「设备管理器 → 端口 (COM 和 LPT)」看有没有黄色感叹号',
            '③ 有感叹号 = 缺驱动：装 CH340 或 CP210x 驱动，装完重新跑本脚本',
            '④ 确认之后可以手动指定：pwsh -File tools\flash.ps1 -Port COM5',
            '⑤ 板子已经在网上了？那就别插线，走无线：pwsh -File tools\flash.ps1 -Ota'
        )
    }

    if ($ports.Count -eq 1) {
        $only = $ports[0]
        Ok ('只探到一个串口，就用它：' + $only.Name + '   ' + $only.Desc)
        return $only.Name
    }

    $usb = @($ports | Where-Object { $_.Score -ge 2 })
    if ($usb.Count -eq 1) {
        Ok ('探到多个串口，挑 USB 串口：' + $usb[0].Name + '   ' + $usb[0].Desc)
        return $usb[0].Name
    }

    Say ''
    Warn '探到多个串口，我不敢替你猜 —— 请用 -Port 指定一个：'
    foreach ($p in $ports) {
        Say ('      ' + $p.Name.PadRight(8) + $p.Desc)
    }
    Fail '需要你指定串口。' @(
        '从上面挑一个 USB 串口（通常描述里带 CH340 / CP210x / FT232 / USB-SERIAL）',
        '然后这样跑：pwsh -File tools\flash.ps1 -Port COM5',
        '或者设环境变量：$env:ROMWATCH_PORT = ''COM5''',
        '拔掉不确定的那个设备再看一遍，端口列表会少一个 —— 就知道哪个是板子了'
    )
}

if ($Help) { Show-Usage; exit 0 }

# ----------------------------------------------------------------- 开场
Say ''
Say 'rom-watch 烧录 —— 编译 + 烧到板子'
Info ('仓库根目录 : ' + $Root)
Info ('固件工程   : ' + $SketchDir)
Info ('板型 FQBN  : ' + $Fqbn + '   （可用 -Fqbn 或环境变量 ROMWATCH_FQBN 覆盖）')
if ($Ota) {
    Info '方式       : 无线 OTA（转调 tools\ota.py push，不碰 USB）'
} else {
    Info '方式       : USB 串口烧录'
}

# ================================================================= 第 1 步：找 arduino-cli
Step '第 1 步 / 共 4 步：找 arduino-cli'

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
    Fail '找不到 arduino-cli，烧不了。' @(
        ('去官网下载（Windows 选 arduino-cli_x.y.z_Windows_64bit.zip）：' + $CliDocUrl),
        '解压出 arduino-cli.exe，放到 tools\arduino-cli\arduino-cli.exe',
        '或者先跑环境准备脚本：pwsh -File tools\setup.ps1'
    )
}

$env:ARDUINO_DIRECTORIES_DATA = $DataDir
$env:ARDUINO_DIRECTORIES_USER = $UserDir

$CliBase = @()
if (Test-Path $CfgFile) { $CliBase += @('--config-file', $CfgFile) }

# ================================================================= 第 2 步：config.h
Step '第 2 步 / 共 4 步：检查 config.h'

$configReady = $false
if (Test-Path $CfgPath) {
    $configReady = $true
    Ok ('找到了：' + $CfgPath)
} else {
    Warn ('还没有 ' + $CfgPath)
    Say ''
    Say '  ★ 建议先补上（二选一）：' -ForegroundColor Yellow
    Say ('     A) copy ' + $CfgExample + ' ' + $CfgPath + '   然后填 WiFi（或把 PVT_WIFI_SET 留 0 走 AP 配网）')
    Say ('     B) python tools\make_config.py')
    Say ''
    Info '★不填也能烧：固件会走中性默认值 + 开机 AP 配网（手机连热点 OTABase-xxx → 打开 192.168.4.1 填 WiFi）。'
}

if (-not (Test-Path $InoPath)) {
    Fail ('没找到固件主文件：' + $InoPath) @(
        '确认 -Sketch 的名字对不对（目录名 = 工程名 = .ino 文件名）',
        '看看 firmware\ 下面有哪些工程：Get-ChildItem firmware'
    )
}

# ================================================================= 第 3 步：编译
Step '第 3 步 / 共 4 步：编译'

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }
Info ('产物目录：' + $BuildDir)

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
        if ($line -match '(?i)error|fatal|not found|undefined') {
            Info $line.TrimEnd()
            $shown = $shown + 1
        }
    }
    if ($shown -eq 0) { Info '（没抓到 error 行，把上面完整输出往上翻一下）' }
    Fail ('编译失败（' + $Fqbn + '，耗时 ' + $secs + 's），还没碰你的板子。') @(
        '① 报 "not found / No such file" ⇒ core 或库没装好：跑 pwsh -File tools\setup.ps1',
        '② 报 IRAM 超了 ⇒ 关掉用不到的功能：加 --build-property "compiler.cpp.extra_flags=-DUSE_TJC=0"',
        '③ config.h 语法错 ⇒ 对着 config.example.h 检查引号有没有配对',
        '④ 实在不行跑 python tools\build_matrix.py，看六种配置哪个能编过'
    )
}
Ok ('编译通过！耗时 ' + $secs + 's')

$rows = [regex]::Matches($text, 'used\s+(\d+)\s*/\s*(\d+)\s*bytes\s*\((\d+)%\)')
if ($rows.Count -ge 3) {
    $labels = @('RAM  ', 'IRAM ', 'Flash')
    for ($i = 0; $i -lt 3; $i++) {
        Info ($labels[$i] + ' : ' + $rows[$i].Groups[1].Value + ' / ' + $rows[$i].Groups[2].Value +
              ' bytes  (' + $rows[$i].Groups[3].Value + '%)')
    }
}

$bin = Join-Path $BuildDir ($Sketch + '.ino.bin')
if (-not (Test-Path $bin)) {
    Warn ('产物目录里没看到 ' + $Sketch + '.ino.bin')
}

# ================================================================= 第 4 步：烧录
Step '第 4 步 / 共 4 步：烧录'

if ($Ota) {
    # ---- 无线 OTA：转调 tools\ota.py push（编译 + 推送 + 校验它自己会做）
    if (-not (Test-Path $OtaPy)) {
        Fail ('没找到 ' + $OtaPy) @('确认你跑脚本的位置：tools\ota.py 应该和本脚本同目录')
    }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $py) {
        Fail '没找到 python（-Ota 要它来跑 tools\ota.py）。' @(
            '装一个 Python 3（装的时候勾上 Add to PATH），然后重跑',
            '或者干脆走 USB：pwsh -File tools\flash.ps1   （插上板子就行）'
        )
    }
    Info ('执行：' + $py.Source + ' ' + $OtaPy + ' push --sketch ' + $Sketch)
    Info '（它会自己找板子 → 编译 → espota 推送 → 校验固件真的换了）'
    Say ''
    try {
        & $py.Source $OtaPy push --sketch $Sketch
        $rc = $LASTEXITCODE
    } catch {
        $rc = 1
    }
    if ($null -eq $rc) { $rc = 0 }
    if ($rc -ne 0) {
        Fail '无线 OTA 失败。' @(
            '① 板子在不在网上？先跑：python tools\ota.py find',
            '② 板子还没配网 / 换了 WiFi ⇒ 跑：python tools\ota.py portal  看配网指引',
            '③ 口令对不上 ⇒ tools\ota.py 会自动读 firmware\' + $Sketch + '\config.h 里的 OTA_PASSWORD，检查那一行',
            '④ 实在刷不进去 ⇒ 插 USB 走串口：pwsh -File tools\flash.ps1 -Port COM5',
            '⑤ 板子刷砖了也能救：python tools\ota.py rescue'
        )
    }
    Ok 'OTA 推送完成'
} else {
    # ---- USB 串口烧录
    if ([string]::IsNullOrWhiteSpace($Port)) {
        Info '没给 -Port，自动探测串口…'
        $Port = Resolve-Port
    } else {
        Ok ('用你指定的串口：' + $Port)
    }

    $known = @(Get-SerialPorts | ForEach-Object { $_.Name })
    if ($known.Count -gt 0 -and ($known -notcontains $Port)) {
        Fail ('串口 ' + $Port + ' 现在不存在（当前有：' + ($known -join ', ') + '）。') @(
            '① 板子插好了吗？设备管理器里能看到这个 COM 口吗',
            '② 有黄色感叹号 = 缺驱动，装 CH340 / CP210x 驱动',
            '③ 换个口试试：pwsh -File tools\flash.ps1 -Port COM7',
            '④ 板子已在网上 ⇒ 走无线：pwsh -File tools\flash.ps1 -Ota'
        )
    }

    Info ('正在烧录 → ' + $Port + '   （上传期间板子上的 LED 会闪，别拔线）')
    $up = Invoke-Cli ($CliBase + @('upload', '-p', $Port, '--fqbn', $Fqbn, '--input-dir', $BuildDir, $SketchDir))
    $upText = [string]$up.Out
    if ($up.Code -ne 0) {
        Say ''
        Say '上传输出里的关键几行：' -ForegroundColor Yellow
        $shown = 0
        foreach ($line in ($upText -split "`n")) {
            if ($shown -ge 15) { break }
            if ($line -match '(?i)error|fail|not found|timed out|denied|busy') {
                Info $line.TrimEnd()
                $shown = $shown + 1
            }
        }
        if ($shown -eq 0) { Info '（没抓到关键行，把上面完整输出往上翻一下）' }
        Fail ('烧录失败（串口 ' + $Port + '）。') @(
            '① "Access is denied / busy" ⇒ 串口被别的程序占着：关掉串口监视器 / Arduino IDE / 其它刷机工具再重试',
            '② "Failed to connect / timed out" ⇒ 按住板子上的 FLASH 键（GPIO0 接地）再点一次上传，或换根 USB 线',
            '③ 板子反复重启、识别不出来 ⇒ 拔掉接在 D6(GPIO12) 上的外设（开机判 flash 电压的脚）',
            '④ 端口号变了 ⇒ 重新探测：pwsh -File tools\flash.ps1',
            '⑤ 板子已经在网上 ⇒ 也可以走无线：pwsh -File tools\flash.ps1 -Ota'
        )
    }
    Ok ('烧录成功 → ' + $Port)
}

# ----------------------------------------------------------------- 收尾
Say ''
Say '下一步：' -ForegroundColor Green
Say '  1) 板子重启后，串口日志里会打印它的 IP'
Say '  2) 浏览器打开那个 IP（就是日志里 "http://xxx.xxx.xxx.xxx/" 那一串）就能看到网页面板'
Say '     ★小提示：板子开机的头几秒，OLED 上也会把这个 IP 亮出来 ✓'
if (-not $Ota) {
    Say ('     想现在就找 IP：pwsh -File tools\flash.ps1 -Monitor    （或 python tools\ota.py serial）')
} else {
    Say '     想找 IP：python tools\ota.py find / python tools\ota.py serial'
}
Say '  3) 网页「监测设置」里填数据源（例：主机 alist.example.com、路径 /public/roms）→ 保存 → 点「立即检查」'
if (-not $configReady) {
    Say ''
    Warn '别忘了：config.h 还没建 —— 没配 WiFi 时板子会开热点（OTABase-xxx），'
    Warn '手机连它 → 打开 http://192.168.4.1/ 填 WiFi 即可。'
}
Say ''
Say '安全提醒：默认 OTA 口令写在源码里，同网段的人都能刷你的固件 ——'
Say '在 firmware\' + $Sketch + '\config.h 里加一行 #define OTA_PASSWORD "换成你自己的"。'
Say ''
exit 0
