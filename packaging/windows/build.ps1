param([Parameter(Mandatory=$true)][string]$Stage,[string]$Version="dev")
$ErrorActionPreference="Stop"; $Root=(Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
python -m pip install --disable-pip-version-check "pyinstaller==6.16.0"
Push-Location $PSScriptRoot
try { python -m PyInstaller --clean --noconfirm dlss-bridge.spec } finally { Pop-Location }
Copy-Item (Join-Path $PSScriptRoot "dist\dlss-bridge.exe") (Join-Path $Stage "dlss-bridge.exe") -Force
$Iscc="${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $Iscc)) { throw "Inno Setup 6 is not installed: $Iscc" }
& $Iscc "/DSourceDir=$Stage" "/DAppVersion=$Version" (Join-Path $PSScriptRoot "dlss-bridge.iss")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
