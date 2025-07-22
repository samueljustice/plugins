#define MyAppName "SammyJs Subbertone"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Samuel Justice"
#define MyAppURL "https://www.sweetjusticesound.com"

[Setup]
AppId={{B4F3A2C1-8D7E-4F5A-9B6C-1A2E3D4C5B6A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={commoncf64}\VST3
DisableDirPage=no
DirExistsWarning=no
AppendDefaultDirName=no
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE
OutputDir=.
OutputBaseFilename=SammyJs_Subbertone_Installer
SetupLogging=yes
Compression=lzma/fast
SolidCompression=no
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Use the packaged VST3 from windows-vst3-package directory
Source: "windows-vst3-package\SammyJs Subbertone.vst3\*"; DestDir: "{app}\SammyJs Subbertone.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not Is64BitInstallMode then
  begin
    MsgBox('This plugin requires a 64-bit version of Windows.', mbError, MB_OK);
    Result := False;
  end;
end;