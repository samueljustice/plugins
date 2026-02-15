[Setup]
AppName=SammyJs Stretch Armstrong
AppVersion=1.0.0
AppPublisher=Samuel Justice
AppPublisherURL=https://www.sweetjusticesound.com
DefaultDirName={commonpf}\Steinberg\VST3
DefaultGroupName=SammyJs Stretch Armstrong
OutputBaseFilename=SammyJs_Stretch_Armstrong_Installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={app}\SammyJs Stretch Armstrong.vst3\Contents\x86_64-win\SammyJs Stretch Armstrong.vst3
LicenseFile=LICENSE

[Files]
Source: "windows-vst3-package\SammyJs Stretch Armstrong.vst3\*"; DestDir: "{app}\SammyJs Stretch Armstrong.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Uninstall SammyJs Stretch Armstrong"; Filename: "{uninstallexe}"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
