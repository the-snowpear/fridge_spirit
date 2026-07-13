# ESP-IDF 环境包装脚本：固定使用本项目已验证的 ESP-IDF 5.5.1。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 <idf.py 参数...>
# 例如：tools\idf.ps1 build / tools\idf.ps1 -p COM15 flash monitor
#
# 2026-07-13：主固件已从 ESP-IDF 6.0.1 降级到 5.5.1，用于恢复 ESP-SR WakeNet less-v3
# 唤醒（P4 rev1.3 的预编译 WakeNet 静态库在 IDF6/GCC15 下链接失败：R_RISCV_RVC_LUI
# relocation truncated）。5.5.1 使用 riscv GCC 14.2.0，与 esp-sr 2.4.5 的 P4 库匹配。
# 版本依据见 doc/IDF5.5迁移与已验证版本.md（厂商 wks_esp32p4_cb_xiaozhi 例程锁定
# IDF 5.5.1 + esp-sr 2.4.5）。IDF6.0.1 仍保留在 D:\esp-IDF\.espressif\v6.0.1，可回退。

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$IdfArgs,
    [Alias('p')][string]$Port,
    [string]$Baud,
    [string]$BuildDir,
    [Alias('C')][string]$ProjectDir,
    [Alias('D')][string[]]$DefineCacheEntry,
    [Alias('dry-run')][switch]$DryRun,
    [switch]$Version,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$IdfRoot = 'D:\esp-IDF\v5.5.1\esp-idf'
$ToolsRoot = 'D:\esp-IDF\.espressif'
$PythonPath = "$ToolsRoot\python_env\idf5.5_py3.14_env\Scripts\python.exe"

if (-not (Test-Path -LiteralPath "$IdfRoot\export.ps1")) {
    throw "Missing project-pinned ESP-IDF 5.5.1: $IdfRoot"
}
if (-not (Test-Path -LiteralPath $ToolsRoot)) {
    throw "Missing ESP-IDF tools directory: $ToolsRoot"
}
if (-not (Test-Path -LiteralPath $PythonPath)) {
    throw "Missing ESP-IDF Python environment: $PythonPath"
}

# Windows PowerShell 5.1 会优先解析带短横线的参数；这里显式收集常用选项，再统一交给 idf.py。
$ForwardArgs = @()
if ($ProjectDir) { $ForwardArgs += @('-C', $ProjectDir) }
if ($BuildDir) { $ForwardArgs += @('-B', $BuildDir) }
if ($Port) { $ForwardArgs += @('-p', $Port) }
if ($Baud) { $ForwardArgs += @('-b', $Baud) }
if ($DefineCacheEntry) {
    foreach ($define in $DefineCacheEntry) { $ForwardArgs += @('-D', $define) }
}
if ($DryRun) { $ForwardArgs += '--dry-run' }
if ($Version) { $ForwardArgs += '--version' }
if ($Help) { $ForwardArgs += '--help' }
$ForwardArgs += $IdfArgs

# 从 Git Bash/MSYS 环境调用时会继承 MSYSTEM 等变量，导致 idf_tools.py 误判为 MSys 环境并拒绝激活。
# 必须通过 Env: 提供程序删除键；SetEnvironmentVariable(..., $null) 在当前宿主会留下空字符串键，
# 而 IDF 5.5 只检查 MSYSTEM 键是否存在，空值也会被判定为不支持的 MSYS 环境。
Remove-Item Env:MSYSTEM, Env:MSYSTEM_CARCH, Env:MSYSTEM_CHOST, Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue

$env:IDF_TOOLS_PATH = $ToolsRoot
# 主线使用 5.5.1；如需临时回退 IDF6，应修改本脚本的固定路径并重新验证工具链。
# 直接调用 activate.py 的导出结果，避免 Windows PowerShell 5.1 对嵌套 dot-source 的作用域差异。
$tempRoot = [System.IO.Path]::GetTempPath()
$activationOutputPath = "$tempRoot\fridge_spirit_idf_activate_$PID.txt"
$activationErrorPath = "$tempRoot\fridge_spirit_idf_activate_$PID.err"
$activationProcess = Start-Process -FilePath $PythonPath -ArgumentList @(
    "$IdfRoot\tools\activate.py",
    '--export'
) -Wait -PassThru -NoNewWindow -RedirectStandardOutput $activationOutputPath -RedirectStandardError $activationErrorPath
if ($activationProcess.ExitCode -ne 0) {
    $activationError = if (Test-Path -LiteralPath $activationErrorPath) { Get-Content -Raw -Encoding UTF8 $activationErrorPath } else { '' }
    Remove-Item -LiteralPath $activationOutputPath,$activationErrorPath -Force -ErrorAction SilentlyContinue
    throw "ESP-IDF environment activation failed: $activationError"
}
$idfExports = Get-Content -Raw -Encoding UTF8 $activationOutputPath
Remove-Item -LiteralPath $activationOutputPath,$activationErrorPath -Force -ErrorAction SilentlyContinue
if ([string]::IsNullOrWhiteSpace($idfExports)) {
    throw "ESP-IDF environment activation failed"
}
Invoke-Expression $idfExports

if ($env:IDF_PATH -ne $IdfRoot) {
    throw "IDF_PATH mismatch: $($env:IDF_PATH)"
}

$idfPy = "$IdfRoot\tools\idf.py"
$idfVersion = (& D:\esp-IDF\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe $idfPy --version | Select-Object -First 1).ToString().Trim()
if ($idfVersion -ne 'ESP-IDF v5.5.1') {
    throw "idf.py version mismatch; expected ESP-IDF v5.5.1, got: $idfVersion"
}

Set-Location $ProjectRoot

# 当前 P4 的 CH343P 在 RAM stub 阶段会出现 Invalid head of packet；默认构建必须生成 no-stub 参数。
$flashActions = @('flash', 'app-flash', 'bootloader-flash', 'partition-table-flash')
$isFlashAction = $false
foreach ($arg in $ForwardArgs) {
    if ($flashActions -contains $arg) {
        $isFlashAction = $true
        break
    }
}
if ($isFlashAction) {
    $flasherArgsPath = Join-Path $ProjectRoot 'build\flasher_args.json'
    if (Test-Path -LiteralPath $flasherArgsPath) {
        $flasherArgs = Get-Content -Raw -Encoding UTF8 $flasherArgsPath | ConvertFrom-Json
        if ($flasherArgs.extra_esptool_args.stub -eq $true) {
            throw "Build still uses the RAM stub. Run tools\idf.ps1 reconfigure build and verify build\flasher_args.json has stub=false before flashing."
        }
    }
}

# ROM loader 的 CH343P 实测以 115200 最稳定；用户显式传入 -b/--baud 时保留用户值。
$hasExplicitBaud = ($ForwardArgs -contains '-b') -or ($ForwardArgs -contains '--baud')
if ($isFlashAction -and -not $hasExplicitBaud) {
    $ForwardArgs = @('-b', '115200') + $ForwardArgs
}

& D:\esp-IDF\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe $idfPy @ForwardArgs
exit $LASTEXITCODE
