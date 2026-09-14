param(
    [string]$Config = "",
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$GameArguments
)
$project = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$controller = Join-Path $project "controller\dlss_bridge.py"
$arguments = @($controller, "run")
if ($Config) { $arguments += @("--config", (Resolve-Path $Config).Path) }
$arguments += "--"
$arguments += $Executable
$arguments += $GameArguments
& python @arguments
exit $LASTEXITCODE
