#ifndef SourceDir
#error SourceDir must point to the staged application
#endif
#ifndef AppVersion
#define AppVersion "dev"
#endif
[Setup]
AppId={{4A2D63C5-9415-4AE5-A7DC-07F3C34F2EA4}
AppName=DLSS Bridge
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\DLSS Bridge
DefaultGroupName=DLSS Bridge
PrivilegesRequired=lowest
ChangesEnvironment=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\..\dist
OutputBaseFilename=dlss-bridge-windows-x86_64-setup
Compression=lzma2
SolidCompression=yes
LicenseFile={#SourceDir}\licenses\bridge-GPL-3.0.txt
[Files]
Source: "{#SourceDir}\dlss-bridge.exe"; DestDir: "{app}"
Source: "{#SourceDir}\payload\*"; DestDir: "{app}\payload"; Flags: recursesubdirs createallsubdirs
Source: "{#SourceDir}\profiles\*"; DestDir: "{app}\profiles"; Flags: recursesubdirs createallsubdirs
Source: "{#SourceDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: recursesubdirs createallsubdirs
Source: "{#SourceDir}\RUNTIME-SOURCE.txt"; DestDir: "{app}"
Source: "{#SourceDir}\runtime-sources.json"; DestDir: "{app}"
Source: "{#SourceDir}\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"
Source: "{#SourceDir}\docs\*"; DestDir: "{app}\docs"; Flags: recursesubdirs createallsubdirs
[Icons]
Name: "{group}\DLSS Bridge"; Filename: "{cmd}"; Parameters: "/K ""{app}\dlss-bridge.exe"" --help"
Name: "{group}\Uninstall DLSS Bridge"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Check: NeedsAddPath(ExpandConstant('{app}')); Flags: preservestringtype

[Code]
function NeedsAddPath(Path: string): Boolean;
var
  CurrentPath: string;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', CurrentPath) then
    CurrentPath := '';
  Result := Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(CurrentPath) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  CurrentPath: string;
  AppPath: string;
begin
  if CurUninstallStep <> usUninstall then
    exit;
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', CurrentPath) then
    exit;
  AppPath := ExpandConstant('{app}');
  StringChangeEx(CurrentPath, ';' + AppPath, '', True);
  StringChangeEx(CurrentPath, AppPath + ';', '', True);
  if CompareText(CurrentPath, AppPath) = 0 then
    CurrentPath := '';
  RegWriteExpandStringValue(HKCU, 'Environment', 'Path', CurrentPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    exit;
  WizardForm.StatusLabel.Caption := 'Downloading and verifying the neural runtime...';
  if not Exec(ExpandConstant('{app}\dlss-bridge.exe'), 'acquire-runtime', '',
              SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Could not start neural runtime acquisition.');
  if ResultCode <> 0 then
    RaiseException(Format('Neural runtime acquisition failed with exit code %d.', [ResultCode]));
end;
