param(
    [Parameter(Mandatory=$true)][string]$Config,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$GameArguments
)
$resolved = (Resolve-Path $Config).Path
$project = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $resolved.StartsWith($project, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Configuration must remain inside the project directory: $project"
}
$env:DLSS_BRIDGE_CONFIG = $resolved
$env:DLSS_BRIDGE_FRONTEND = "windows"
& $Executable @GameArguments
exit $LASTEXITCODE
