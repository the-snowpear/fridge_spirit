# ESP-IDF 环境包装脚本：按项目规则设置 IDF_TOOLS_PATH 后再调用 idf.py。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 <idf.py 参数...>
# 例如：tools\idf.ps1 build / tools\idf.ps1 -p COM7 flash monitor
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$IdfArgs)

# 从 Git Bash/MSYS 环境调用时会继承 MSYSTEM 等变量，导致 idf_tools.py 误判为 MSys 环境并拒绝激活。
# 注意：Remove-Item Env: 对继承变量在子进程中可能不生效，必须用 .NET API 清除。
[Environment]::SetEnvironmentVariable('MSYSTEM', $null)
[Environment]::SetEnvironmentVariable('MSYSTEM_CARCH', $null)
[Environment]::SetEnvironmentVariable('MSYSTEM_CHOST', $null)
[Environment]::SetEnvironmentVariable('MSYSTEM_PREFIX', $null)

$env:IDF_TOOLS_PATH = 'D:\esp-IDF\.espressif'
. D:\esp-IDF\.espressif\v6.0.1\esp-idf\export.ps1
Set-Location $PSScriptRoot\..
idf.py @IdfArgs
exit $LASTEXITCODE
