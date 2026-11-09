; Roarschach for Windows: the VST3 plug-in, in Common Files\VST3.
; Built by .github/workflows/build-release_roarschach.yml, which passes the version:
;   iscc /DMyAppVersion=1.2.3 installer.iss
#ifndef MyAppVersion
  #define MyAppVersion "1.0.1"
#endif
#define MyAppName "Roarschach"
#define MyAppPublisher "Sammy J"
#define MyAppURL "https://www.sweetjusticesound.com"
#define MyBundle "Roarschach.vst3"

[Setup]
; Never change the AppId: it is how an update finds and replaces the installed copy (and how the
; combined Sammy J installer finds this one to remove it first).
AppId={{EDA89529-4B87-433C-AC72-15F7FBDFC421}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
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
OutputBaseFilename=Roarschach_Installer
SetupLogging=yes
Compression=lzma2
SolidCompression=yes
; x64compatible also covers Windows on Arm, where x64 hosts run under emulation.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[InstallDelete]
; The old copy goes first, so files a newer version no longer has don't linger.
Type: filesandordirs; Name: "{app}\{#MyBundle}"

[Files]
Source: "windows-package\vst3\{#MyBundle}\*"; DestDir: "{app}\{#MyBundle}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
