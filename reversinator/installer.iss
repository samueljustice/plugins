[Setup]
AppName=SammyJs Reversinator
AppVersion=1.0.0
DefaultDirName={commoncf64}\VST3
DefaultGroupName=SammyJs Reversinator
OutputBaseFilename=SammyJs_Reversinator_Installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\SammyJs Reversinator.vst3
LicenseFile=LICENSE
AppPublisher=Samuel Justice
AppPublisherURL=https://www.sweetjusticesound.com
AppSupportURL=https://www.sweetjusticesound.com
AppUpdatesURL=https://www.sweetjusticesound.com

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "windows-vst3-package\SammyJs Reversinator.vst3\*"; DestDir: "{commoncf64}\VST3\SammyJs Reversinator.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{cm:UninstallProgram,SammyJs Reversinator}"; Filename: "{uninstallexe}"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;