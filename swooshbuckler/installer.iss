; Swooshbuckler for Windows: the app (Program Files) and the VST3 plug-in (Common Files\VST3).
; Built by .github/workflows/build-release_swooshbuckler.yml, which passes the version:
;   iscc /DMyAppVersion=1.2.3 installer.iss
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define MyAppName "Swooshbuckler"
#define MyAppPublisher "Sammy J"
#define MyAppURL "https://www.sweetjusticesound.com"
#define MyAppExe "Swooshbuckler.exe"

[Setup]
AppId={{7D3E5B1A-4C2F-4E8B-9A61-5B2C8D0F3E47}
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
OutputBaseFilename=Swooshbuckler_Installer
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
Name: "full"; Description: "App and VST3 plug-in"
Name: "app"; Description: "App only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "app"; Description: "Swooshbuckler app"; Types: full app custom
Name: "vst3"; Description: "VST3 plug-in (Common Files\VST3)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: app; Flags: unchecked

[Files]
Source: "windows-package\app\{#MyAppExe}"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "windows-package\vst3\Swooshbuckler.vst3\*"; DestDir: "{commoncf64}\VST3\Swooshbuckler.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Components: app
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Components: app; Flags: nowait postinstall skipifsilent
