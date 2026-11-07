#define MyAppName "Nord Buds"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "kmvishn"
#define MyAppURL "https://github.com/kmvishn/nord-buds-companion"
#define MyAppExeName "nordbuds.exe"

[Setup]
AppId={{D37BF9E4-1E06-4447-B9C0-5C6DDF6F7B8A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={localappdata}\NordBuds
DisableDirPage=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=NordBuds-Setup
SetupIconFile=res\nordbuds.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addtopath"; Description: "Add Nord Buds to PATH (for command-line access)"; GroupDescription: "Command Line:"
Name: "autostart"; Description: "Start Nord Buds automatically when signing in to Windows"; GroupDescription: "Startup:"

[Files]
Source: "build\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\nordbuds.com"; DestDir: "{app}"; Flags: ignoreversion
Source: "res\nordbuds.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\nordbuds.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\nordbuds.ico"; Tasks: desktopicon

[Registry]
; Start with Windows
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "NordBuds"; ValueData: """{app}\{#MyAppExeName}"" --background"; Flags: uninsdeletevalue; Tasks: autostart

; Add to User PATH
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Quit Nord Buds before uninstalling
Filename: "{app}\nordbuds.com"; Parameters: "quit"; Flags: runhidden; RunOnceId: "QuitNordBuds"

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath)
  then begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Param + ';', ';' + OrigPath + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir, OrigPath, NewPath: string;
  P, L: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    AppDir := ExpandConstant('{app}');
    if RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
    begin
      P := Pos(';' + AppDir + ';', ';' + OrigPath + ';');
      if P > 0 then
      begin
        NewPath := ';' + OrigPath + ';';
        L := Length(AppDir) + 1;
        Delete(NewPath, P, L);
        if (Length(NewPath) > 0) and (NewPath[1] = ';') then
          Delete(NewPath, 1, 1);
        if (Length(NewPath) > 0) and (NewPath[Length(NewPath)] = ';') then
          Delete(NewPath, Length(NewPath), 1);
        RegWriteStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', NewPath);
      end;
    end;
  end;
end;
