[CmdletBinding()]
param(
    [string]$Port = "COM17",
    [int]$Baudrate = 115200,
    [string]$Output = "tmp\cache\screenshot_test\codex_latest",
    [string]$Page = "",
    [string]$Orientation = "",
    [double]$ReadyTimeoutSeconds = 30,
    [double]$ResourceTimeoutSeconds = 45,
    [double]$CaptureDelaySeconds = 0.25
)

# 项目根目录唯一截屏入口：转发到 tmp/tools，避免 Codex/协作者各自重建串口协议。
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$implementation = Join-Path $projectRoot "tmp\tools\capture_screenshot.ps1"
if (-not (Test-Path -LiteralPath $implementation)) {
    throw "找不到截屏实现：$implementation"
}

$forwardArgs = @{
    Port = $Port
    Baudrate = $Baudrate
    Output = $Output
    ReadyTimeoutSeconds = $ReadyTimeoutSeconds
    ResourceTimeoutSeconds = $ResourceTimeoutSeconds
    CaptureDelaySeconds = $CaptureDelaySeconds
}
if ($Page) {
    $forwardArgs.Page = $Page
}
if ($Orientation) {
    $forwardArgs.Orientation = $Orientation
}

& $implementation @forwardArgs
exit $LASTEXITCODE
