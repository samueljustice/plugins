; Sammy J's Plugins for Windows: every plug-in's VST3, and the apps that have one, in one installer.
; Built by .github/workflows/build-release_bundle.yml from each plug-in's newest release, which lays the
; files out as windows-package\<id>\vst3\<Bundle>.vst3 and windows-package\<id>\app\<App>.exe and passes:
;   iscc /DBundleVersion=2026.11.7.1 /DSwooshbucklerVersion=1.0.0 /DSoundAdviceVersion=1.0.0
;        /DSubbertoneVersion=1.2.0 /DPitchFlattenerVersion=2.0.0 /DReversinatorVersion=2.0.0
;        /DRoarschachVersion=1.0.0 installer.iss
;
; Old versions: before installing a plug-in it silently runs that plug-in's own (stand-alone) uninstaller
; if one is registered, then deletes the old .vst3 folders, so no stale files or duplicate plug-ins are
; left. Adding a plug-in: a line in bundle/plugins.txt, plus components, files and an uninstall call here.

#ifndef BundleVersion
  #define BundleVersion "0.0.0.0"
#endif
#ifndef SwooshbucklerVersion
  #define SwooshbucklerVersion "?"
#endif
#ifndef SoundAdviceVersion
  #define SoundAdviceVersion "?"
#endif
#ifndef SubbertoneVersion
  #define SubbertoneVersion "?"
#endif
#ifndef PitchFlattenerVersion
  #define PitchFlattenerVersion "?"
#endif
#ifndef ReversinatorVersion
  #define ReversinatorVersion "?"
#endif
#ifndef RoarschachVersion
  #define RoarschachVersion "?"
#endif

#define MyAppName "Sammy J's Plugins"
#define MyAppPublisher "Sammy J"
#define MyAppURL "https://github.com/samueljustice/plugins"
#define VST3Dir "{commoncf64}\VST3"
#define UserVST3Dir "{localappdata}\Programs\Common\VST3"

[Setup]
AppId={{E6B1C7D4-2A9F-4F3E-8B5C-7D0A1E9F4C62}
AppName={#MyAppName}
AppVersion={#BundleVersion}
AppVerName={#MyAppName} {#BundleVersion}
UninstallDisplayName={#MyAppName}
VersionInfoVersion={#BundleVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases/latest
DefaultDirName={autopf}\Sammy J
DefaultGroupName=Sammy J
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename=SammyJ-Plugins-Windows-Installer
SetupLogging=yes
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Everything"
Name: "custom"; Description: "Choose plug-ins"; Flags: iscustom

[Components]
Name: "swooshbuckler"; Description: "Swooshbuckler {#SwooshbucklerVersion}"; Types: full
Name: "swooshbuckler\app"; Description: "App"; Types: full
Name: "swooshbuckler\vst3"; Description: "VST3 plug-in"; Types: full
Name: "soundadvice"; Description: "Sound Advice {#SoundAdviceVersion}"; Types: full
Name: "soundadvice\app"; Description: "App"; Types: full
Name: "soundadvice\vst3"; Description: "VST3 plug-in"; Types: full
Name: "subbertone"; Description: "Subbertone {#SubbertoneVersion} (VST3)"; Types: full
Name: "pitchflattener"; Description: "Pitch Flattener {#PitchFlattenerVersion} (VST3)"; Types: full
Name: "reversinator"; Description: "Reversinator {#ReversinatorVersion} (VST3)"; Types: full
Name: "roarschach"; Description: "Roarschach {#RoarschachVersion} (VST3)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: swooshbuckler\app soundadvice\app; Flags: unchecked

[InstallDelete]
; The old copy goes first, so files a newer version no longer has don't linger; a per-user copy would
; show up twice in hosts.
Type: filesandordirs; Name: "{#VST3Dir}\Swooshbuckler.vst3"; Components: swooshbuckler\vst3
Type: filesandordirs; Name: "{#UserVST3Dir}\Swooshbuckler.vst3"; Components: swooshbuckler\vst3
Type: filesandordirs; Name: "{#VST3Dir}\Sound Advice.vst3"; Components: soundadvice\vst3
Type: filesandordirs; Name: "{#UserVST3Dir}\Sound Advice.vst3"; Components: soundadvice\vst3
Type: filesandordirs; Name: "{#VST3Dir}\SammyJs Subbertone.vst3"; Components: subbertone
Type: filesandordirs; Name: "{#UserVST3Dir}\SammyJs Subbertone.vst3"; Components: subbertone
Type: filesandordirs; Name: "{#VST3Dir}\SammyJs Pitch Flattener.vst3"; Components: pitchflattener
Type: filesandordirs; Name: "{#UserVST3Dir}\SammyJs Pitch Flattener.vst3"; Components: pitchflattener
Type: filesandordirs; Name: "{#VST3Dir}\SammyJs Reversinator.vst3"; Components: reversinator
Type: filesandordirs; Name: "{#UserVST3Dir}\SammyJs Reversinator.vst3"; Components: reversinator
Type: filesandordirs; Name: "{#VST3Dir}\Roarschach.vst3"; Components: roarschach
Type: filesandordirs; Name: "{#UserVST3Dir}\Roarschach.vst3"; Components: roarschach

[UninstallDelete]
; Per-user copies of the plug-ins (the installer removes these too, so hosts don't list them twice), and
; the Sammy J folder once the apps are gone.
Type: filesandordirs; Name: "{#UserVST3Dir}\Swooshbuckler.vst3"
Type: filesandordirs; Name: "{#UserVST3Dir}\Sound Advice.vst3"
Type: filesandordirs; Name: "{#UserVST3Dir}\SammyJs Subbertone.vst3"
Type: filesandordirs; Name: "{#UserVST3Dir}\SammyJs Pitch Flattener.vst3"
Type: filesandordirs; Name: "{#UserVST3Dir}\SammyJs Reversinator.vst3"
Type: filesandordirs; Name: "{#UserVST3Dir}\Roarschach.vst3"
Type: dirifempty; Name: "{app}\Swooshbuckler"
Type: dirifempty; Name: "{app}\Sound Advice"
Type: dirifempty; Name: "{app}"

[Files]
Source: "windows-package\swooshbuckler\app\Swooshbuckler.exe"; DestDir: "{app}\Swooshbuckler"; Components: swooshbuckler\app; Flags: ignoreversion
Source: "windows-package\swooshbuckler\vst3\Swooshbuckler.vst3\*"; DestDir: "{#VST3Dir}\Swooshbuckler.vst3"; Components: swooshbuckler\vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "windows-package\soundadvice\app\Sound Advice.exe"; DestDir: "{app}\Sound Advice"; Components: soundadvice\app; Flags: ignoreversion
Source: "windows-package\soundadvice\vst3\Sound Advice.vst3\*"; DestDir: "{#VST3Dir}\Sound Advice.vst3"; Components: soundadvice\vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "windows-package\subbertone\vst3\SammyJs Subbertone.vst3\*"; DestDir: "{#VST3Dir}\SammyJs Subbertone.vst3"; Components: subbertone; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "windows-package\pitchflattener\vst3\SammyJs Pitch Flattener.vst3\*"; DestDir: "{#VST3Dir}\SammyJs Pitch Flattener.vst3"; Components: pitchflattener; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "windows-package\reversinator\vst3\SammyJs Reversinator.vst3\*"; DestDir: "{#VST3Dir}\SammyJs Reversinator.vst3"; Components: reversinator; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "windows-package\roarschach\vst3\Roarschach.vst3\*"; DestDir: "{#VST3Dir}\Roarschach.vst3"; Components: roarschach; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Swooshbuckler"; Filename: "{app}\Swooshbuckler\Swooshbuckler.exe"; Components: swooshbuckler\app
Name: "{group}\Sound Advice"; Filename: "{app}\Sound Advice\Sound Advice.exe"; Components: soundadvice\app
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Swooshbuckler"; Filename: "{app}\Swooshbuckler\Swooshbuckler.exe"; Components: swooshbuckler\app; Tasks: desktopicon
Name: "{autodesktop}\Sound Advice"; Filename: "{app}\Sound Advice\Sound Advice.exe"; Components: soundadvice\app; Tasks: desktopicon

[Code]
{ Runs a plug-in's old stand-alone installer's uninstaller, if one is registered, and waits for it. }
procedure RunOldUninstaller(const AppId: String);
var
  Key, Cmd: String;
  Code: Integer;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\' + AppId + '_is1';
  if RegQueryStringValue(HKLM64, Key, 'UninstallString', Cmd) or
     RegQueryStringValue(HKLM32, Key, 'UninstallString', Cmd) or
     RegQueryStringValue(HKCU, Key, 'UninstallString', Cmd) then
  begin
    Cmd := RemoveQuotes(Cmd);
    if FileExists(Cmd) then
    begin
      Log('Removing the old installer for ' + AppId + ': ' + Cmd);
      if not Exec(Cmd, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, Code) then
        Log('Could not run it: ' + SysErrorMessage(Code));
    end;
  end;
end;

{ Uninstalling: settings and presets go only if the user says so. Recordings and anything else saved
  where the user chose are never touched. The macOS list is bundle/userdata.txt. }
var
  RemoveUserData: Boolean;

procedure DeleteIfEmpty(const Dir: String);
begin
  if DirExists(Dir) then
    if RemoveDir(Dir) then Log('Removed empty folder ' + Dir);
end;

procedure RemoveUserFile(const Path: String);
begin
  if FileExists(Path) then
    if DeleteFile(Path) then Log('Removed ' + Path);
end;

procedure RemoveUserFolder(const Path: String);
begin
  if DirExists(Path) then
    if DelTree(Path, True, True, True) then Log('Removed ' + Path);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usUninstall then
    RemoveUserData := (not UninstallSilent) and
      (MsgBox('Also remove your settings and saved presets?' + #13#10#13#10 +
              'Keep them if you might reinstall later.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES);

  if (CurUninstallStep = usPostUninstall) and RemoveUserData then
  begin
    Data := ExpandConstant('{userappdata}');
    RemoveUserFile(Data + '\Sweet Justice\Swooshbuckler.settings');
    RemoveUserFile(Data + '\Sweet Justice\Sound Advice.settings');
    RemoveUserFolder(Data + '\SammyJs\Subbertone');
    RemoveUserFolder(Data + '\SammyJs\PitchFlattener');
    RemoveUserFolder(Data + '\SammyJs\Roarschach');
    { The vendor folders are shared with other plug-ins, so they go only when nothing is left in them. }
    DeleteIfEmpty(Data + '\Sweet Justice');
    DeleteIfEmpty(Data + '\SammyJs');
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { The AppIds of each plug-in's own installer.iss. }
  if WizardIsComponentSelected('swooshbuckler\app or swooshbuckler\vst3') then
    RunOldUninstaller('{7D3E5B1A-4C2F-4E8B-9A61-5B2C8D0F3E47}');
  if WizardIsComponentSelected('soundadvice\app or soundadvice\vst3') then
    RunOldUninstaller('{C2A8E6F1-3B9D-4A57-8E2C-6D1F0B7A9C35}');
  if WizardIsComponentSelected('subbertone') then
    RunOldUninstaller('{B4F3A2C1-8D7E-4F5A-9B6C-1A2E3D4C5B6A}');
  if WizardIsComponentSelected('pitchflattener') then
    RunOldUninstaller('{85CA9E2F-3B5A-4D1B-9C5E-8F5A7B2C3D4E}');
  if WizardIsComponentSelected('reversinator') then
    RunOldUninstaller('{A5D8E3F2-4C6B-5E2D-8F6C-9B3A5D1E7F8B}');
  if WizardIsComponentSelected('roarschach') then
    RunOldUninstaller('{EDA89529-4B87-433C-AC72-15F7FBDFC421}');
  Result := '';
end;
