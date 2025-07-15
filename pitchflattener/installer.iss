#define MyAppName "SammyJs Pitch Flattener"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Samuel Justice"
#define MyAppURL "https://github.com/samueljustice/pitchflattener"

[Setup]
AppId={{85CA9E2F-3B5A-4D1B-9C5E-8F5A7B2C3D4E}
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
OutputBaseFilename=SammyJs_Pitch_Flattener_Installer
SetupLogging=yes
Compression=lzma/fast
SolidCompression=no
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Try to copy from VST3 bundle directory first
Source: "build\PitchFlattener_artefacts\Release\VST3\SammyJs Pitch Flattener.vst3\*"; DestDir: "{app}\SammyJs Pitch Flattener.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: DirExists(ExpandConstant('{#SourcePath}\build\PitchFlattener_artefacts\Release\VST3\SammyJs Pitch Flattener.vst3'))
; If no bundle exists, copy from windows-vst3-package
Source: "windows-vst3-package\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: DirExists(ExpandConstant('{#SourcePath}\windows-vst3-package'))

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