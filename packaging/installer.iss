; MAVIRA - Native C++20 Batch Comic Converter, Extractor, Merger & Offline OCR Suite
; Windows x64 Inno Setup Installer Script

#define MyAppName "MAVIRA"
#define MyAppVersion "1.1.0"
#define MyAppPublisher "MAVIRA Contributors"
#define MyAppExeName "MAVIRA.exe"
#define RootDir ".."
#define RuntimeDir "..\runtime"

[Setup]
AppId={{8D4F6E2A-9B1C-4C3A-B2D7-7F5A1E4C9A10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppComments=Native C++20 Batch Comic Converter, Extractor, Merger & Offline OCR Suite with Built-In Quick Preview Reader
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
InfoAfterFile=..\THIRD_PARTY_NOTICES.txt
OutputDir=..\release
OutputBaseFilename=MAVIRA-Setup-x64-v1.1.0
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#RootDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RuntimeDir}\*"; DestDir: "{app}\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RootDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\data"; Permissions: users-modify

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
