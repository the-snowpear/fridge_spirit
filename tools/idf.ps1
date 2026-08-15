# ESP-IDF 环境包装脚本：固定使用本项目已验证的 ESP-IDF 5.5.1。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 <idf.py 参数...>
# 例如：tools\idf.ps1 build / tools\idf.ps1 -p COM17 flash monitor

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$IdfArgs,
    [Alias('p')][string]$Port,
    [string]$Baud = '115200',
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
$PythonCandidates = @(
    "$ToolsRoot\python_env\idf5.5_py3.14_env\Scripts\python.exe",
    "$ProjectRoot\tmp\cache\idf_venv\Scripts\python.exe"
)
$PythonPath = $PythonCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not (Test-Path -LiteralPath "$IdfRoot\export.ps1")) {
    throw "Missing project-pinned ESP-IDF 5.5.1: $IdfRoot"
}
if (-not (Test-Path -LiteralPath $ToolsRoot)) {
    throw "Missing ESP-IDF tools directory: $ToolsRoot"
}
# 优先使用构建目录已记录的官方 IDF 5.5.1 Python 3.14 环境，避免 CMake 拒绝混用解释器。
# 项目内 Python 3.9 环境仅用于官方环境不可用时的兼容回退。
# Keep this ASCII guard line so Windows PowerShell 5 cannot merge the next statement.
if (-not $PythonPath) {
    throw "Missing usable ESP-IDF Python environment: $($PythonCandidates -join ', ')"
}

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

# Git Bash/MSYS 环境变量会让 IDF 5.5 误判当前宿主，激活前必须从当前进程移除。
Remove-Item Env:MSYSTEM, Env:MSYSTEM_CARCH, Env:MSYSTEM_CHOST, Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue

$env:IDF_TOOLS_PATH = $ToolsRoot
$env:IDF_PYTHON_ENV_PATH = Split-Path -Parent (Split-Path -Parent $PythonPath)
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
    throw 'ESP-IDF environment activation failed'
}
Invoke-Expression $idfExports

if ($env:IDF_PATH -ne $IdfRoot) {
    throw "IDF_PATH mismatch: $($env:IDF_PATH)"
}

$idfPy = "$IdfRoot\tools\idf.py"
$idfVersion = (& $PythonPath $idfPy --version | Select-Object -First 1).ToString().Trim()
if ($idfVersion -ne 'ESP-IDF v5.5.1') {
    throw "idf.py version mismatch; expected ESP-IDF v5.5.1, got: $idfVersion"
}

Set-Location $ProjectRoot

# P4/CH343P 当前必须使用 ROM loader；烧录前阻止误用 RAM stub。
$flashActions = @('flash', 'app-flash', 'bootloader-flash', 'partition-table-flash')
$isFlashAction = (($ForwardArgs -join "`n") -match
    '(?m)^(flash|app-flash|bootloader-flash|partition-table-flash)$')
if ($isFlashAction) {
    $flasherArgsPath = Join-Path $ProjectRoot 'build\flasher_args.json'
    if (Test-Path -LiteralPath $flasherArgsPath) {
        $flasherArgs = Get-Content -Raw -Encoding UTF8 $flasherArgsPath | ConvertFrom-Json
        if ($flasherArgs.extra_esptool_args.stub -eq $true) {
            throw 'Build still uses the RAM stub. Run tools\idf.ps1 reconfigure build and verify build\flasher_args.json has stub=false before flashing.'
        }
    }
}

if ($isFlashAction -or $DryRun) {
    Write-Host "idf.py effective args: $($ForwardArgs -join ' ')"
}
& $PythonPath $idfPy @ForwardArgs
exit $LASTEXITCODE
