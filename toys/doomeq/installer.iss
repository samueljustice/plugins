; DOOM EQ for Windows: the app (Program Files), the VST3 plug-in (Common Files\VST3) and, when the build
; passes DoomEQAAX, the PACE-signed AAX plug-in for Pro Tools (Common Files\Avid\Audio\Plug-Ins).
; Built by .github/workflows/build-release_doomeq.yml (no AAX: it isn't signed yet when the release is made)
; and by .github/workflows/doomeq-test-installers.yml (with the signed AAX), which pass the version:
;   iscc /DMyAppVersion=1.2.3 [/DDoomEQAAX] installer.iss
; It expects windows-package\app\SammyJs DOOM EQ.exe, windows-package\vst3\SammyJs DOOM EQ.vst3 and, with DoomEQAAX,
; windows-package\aax\SammyJs DOOM EQ.aaxplugin.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.1"
#endif
#define MyAppName "DOOM EQ"
#define MyAppPublisher "Sammy J"
#define MyAppURL "https://github.com/samueljustice/plugins"
#define MyAppExe "SammyJs DOOM EQ.exe"
#define MyBundle "SammyJs DOOM EQ"
#define VST3Dir "{commoncf64}\VST3"
#define AAXDir "{commoncf64}\Avid\Audio\Plug-Ins"

[Setup]
; Never change the AppId: it is how an update finds and replaces the installed copy.
AppId={{8AC23415-9961-4146-AE36-E5104268F578}
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
OutputBaseFilename=DoomEQ_Installer
UninstallDisplayIcon={app}\{#MyAppExe}
SetupLogging=yes
Compression=lzma2
SolidCompression=yes
; x64compatible also covers Windows on Arm, where x64 hosts run under emulation.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Everything"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in (Common Files\VST3)"; Types: full
#ifdef DoomEQAAX
Name: "aax"; Description: "AAX plug-in for Pro Tools (Common Files\Avid\Audio\Plug-Ins)"; Types: full
#endif
Name: "app"; Description: "DOOM EQ app"; Types: full

[InstallDelete]
; The old copy goes first, so files a newer version no longer has don't linger.
Type: filesandordirs; Name: "{#VST3Dir}\{#MyBundle}.vst3"; Components: vst3
#ifdef DoomEQAAX
Type: filesandordirs; Name: "{#AAXDir}\{#MyBundle}.aaxplugin"; Components: aax
#endif

[Files]
Source: "windows-package\vst3\{#MyBundle}.vst3\*"; DestDir: "{#VST3Dir}\{#MyBundle}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef DoomEQAAX
Source: "windows-package\aax\{#MyBundle}.aaxplugin\*"; DestDir: "{#AAXDir}\{#MyBundle}.aaxplugin"; Components: aax; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
Source: "windows-package\app\{#MyAppExe}"; DestDir: "{app}"; Components: app; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Components: app
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
