; Sound Advice for Windows: the VST3 plug-in (Common Files\VST3) and the app (Program Files).
; Built by .github/workflows/build-release_soundadvice.yml, which passes the version:
;   iscc /DMyAppVersion=1.2.3 installer.iss
#ifndef MyAppVersion
  #define MyAppVersion "1.0.11"
#endif
#define MyAppName "Sound Advice"
#define MyAppPublisher "Sammy J"
#define MyAppURL "https://www.sweetjusticesound.com"
#define MyAppExe "Sound Advice.exe"

[Setup]
AppId={{C2A8E6F1-3B9D-4A57-8E2C-6D1F0B7A9C35}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Sammy J\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE
OutputDir=.
OutputBaseFilename=SoundAdvice_Installer
SetupIconFile=Resources\icon.ico
UninstallDisplayIcon={app}\{#MyAppExe}
SetupLogging=yes
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "VST3 plug-in and app"
Name: "plugin"; Description: "VST3 plug-in only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in (Common Files\VST3)"; Types: full plugin custom
Name: "app"; Description: "Sound Advice app"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: app; Flags: unchecked

[Files]
Source: "windows-package\app\{#MyAppExe}"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "windows-package\vst3\Sound Advice.vst3\*"; DestDir: "{commoncf64}\VST3\Sound Advice.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Components: app
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Components: app; Flags: nowait postinstall skipifsilent unchecked
