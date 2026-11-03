; Direct Tuner Recorder installer. Build with: ISCC installer\recorder.iss
; Expects build\DirectTunerRecorder.exe.

#define AppVersion "1.0.0"

[Setup]
AppId={{6E1C9B54-3C1E-4C47-9B0A-2D7C2C9E1A11}
AppName=Direct Tuner Recorder
AppVersion={#AppVersion}
AppPublisher=ferrellsl
AppPublisherURL=https://github.com/ferrellsl/Direct-Tuner-Recorder
DefaultDirName={autopf}\Direct Tuner Recorder
DefaultGroupName=Direct Tuner Recorder
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir=..\dist
OutputBaseFilename=DirectTunerRecorder-{#AppVersion}-setup
LicenseFile=..\LICENSE.md
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=Direct Tuner Recorder
UninstallDisplayIcon={app}\DirectTunerRecorder.exe

[Files]
Source: "..\build\DirectTunerRecorder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE.md"; DestDir: "{app}"

[Dirs]
Name: "{commonappdata}\Direct Tuner Recorder"

[Icons]
Name: "{group}\Recorder log"; Filename: "{commonappdata}\Direct Tuner Recorder\recorder.log"
Name: "{group}\Recorder settings"; Filename: "notepad.exe"; Parameters: """{commonappdata}\Direct Tuner Recorder\recorder.json"""
Name: "{group}\Recordings folder"; Filename: "{code:GetRecordingsFolder}"
Name: "{group}\Uninstall Direct Tuner Recorder"; Filename: "{uninstallexe}"

[UninstallRun]
Filename: "{app}\DirectTunerRecorder.exe"; Parameters: "--uninstall"; Flags: runhidden; RunOnceId: "RemoveService"
Filename: "netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Direct Tuner Recorder"""; Flags: runhidden; RunOnceId: "RemoveFirewall"

[Code]
var
  FolderPage: TInputDirWizardPage;
  GuidePage: TInputFileWizardPage;
  AccountPage: TInputQueryWizardPage;
  ExistingSettings: String;

function DataFolder(): String;
begin
  Result := ExpandConstant('{commonappdata}\Direct Tuner Recorder');
end;

// The value of a string setting in recorder.json, or Default.
function JsonValue(const Key, Default: String): String;
var
  P, E: Integer;
  S: String;
begin
  Result := Default;
  P := Pos('"' + Key + '"', ExistingSettings);
  if P = 0 then Exit;
  S := Copy(ExistingSettings, P + Length(Key) + 2, MaxInt);
  P := Pos('"', S);
  if P = 0 then Exit;
  S := Copy(S, P + 1, MaxInt);
  E := Pos('"', S);
  if E = 0 then Exit;
  S := Copy(S, 1, E - 1);
  StringChangeEx(S, '\\', '\', True);
  Result := S;
end;

function JsonEscape(const S: String): String;
begin
  Result := S;
  StringChangeEx(Result, '\', '\\', True);
  StringChangeEx(Result, '"', '\"', True);
end;

function GetRecordingsFolder(Param: String): String;
begin
  Result := FolderPage.Values[0];
end;

function IsNetworkFolder(): Boolean;
begin
  Result := Copy(FolderPage.Values[0], 1, 2) = '\\';
end;

procedure InitializeWizard();
var
  Settings: AnsiString;
  Guide: String;
begin
  if LoadStringFromFile(DataFolder() + '\recorder.json', Settings) then
    ExistingSettings := String(Settings);

  FolderPage := CreateInputDirPage(wpSelectDir, 'Recordings folder',
    'Where should recordings be saved?',
    'Every Kodi that uses this recorder plays recordings from here. Choose a local drive with plenty ' +
    'of space, or a network share such as \\nas\TV. An hour of HD is about 3 to 7 GB.',
    False, '');
  FolderPage.Add('');
  FolderPage.Values[0] := JsonValue('recordingsFolder',
    ExpandConstant('{%PUBLIC|C:\Users\Public}') + '\Videos\Kodi Recordings');

  AccountPage := CreateInputQueryPage(FolderPage.ID, 'Network folder account',
    'Which Windows account can write to the network folder?',
    'The recorder runs in the background. To save to a network share it needs a Windows account ' +
    'that can reach it, usually your own. The password is stored by Windows, not by the recorder.');
  AccountPage.Add('Account name, such as ' + GetUserNameString + ':', False);
  AccountPage.Add('Password:', True);
  AccountPage.Values[0] := GetUserNameString;

  GuidePage := CreateInputFilePage(AccountPage.ID, 'Guide',
    'Do you have an XMLTV guide file?',
    'With no file, the guide comes from the tuner and covers about 2 days. If you make an XMLTV file, ' +
    'for example with OneGuide, choose it here to use it for every channel it covers. Leave it empty if not.');
  GuidePage.Add('XMLTV guide file (optional):', 'XMLTV files (*.xml)|*.xml|All files|*.*', '.xml');
  Guide := JsonValue('guideFile', '');
  if (Guide = '') and FileExists('C:\Zap2XML\xmltv.xml') then
    Guide := 'C:\Zap2XML\xmltv.xml';
  GuidePage.Values[0] := Guide;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = AccountPage.ID) and not IsNetworkFolder();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = AccountPage.ID) and ((Trim(AccountPage.Values[0]) = '') or (AccountPage.Values[1] = '')) then
  begin
    MsgBox('Enter the account name and its password.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Json, Account, Folder: String;
begin
  if CurStep = ssInstall then
  begin
    // An upgrade replaces a running program, so stop it first.
    if FileExists(ExpandConstant('{app}\DirectTunerRecorder.exe')) then
      Exec(ExpandConstant('{app}\DirectTunerRecorder.exe'), '--stop', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;

  if CurStep = ssPostInstall then
  begin
    Folder := FolderPage.Values[0];
    if not IsNetworkFolder() then
      ForceDirectories(Folder);

    Json := '{' + #13#10 +
      '  "recordingsFolder": "' + JsonEscape(Folder) + '",' + #13#10 +
      '  "tunerAddress": "' + JsonEscape(JsonValue('tunerAddress', '')) + '",' + #13#10 +
      '  "guideFile": "' + JsonEscape(GuidePage.Values[0]) + '",' + #13#10 +
      '  "hideCopyProtected": true,' + #13#10 +
      '  "port": 8866,' + #13#10 +
      '  "allowNetwork": true' + #13#10 + '}' + #13#10;
    SaveStringToFile(DataFolder() + '\recorder.json', Json, False);

    // Kodi on other computers reaches the recorder on port 8866: TCP for the API, UDP to find it.
    Exec('netsh.exe', 'advfirewall firewall delete rule name="Direct Tuner Recorder"', '', SW_HIDE,
      ewWaitUntilTerminated, Code);
    Exec('netsh.exe', 'advfirewall firewall add rule name="Direct Tuner Recorder" dir=in action=allow ' +
      'protocol=TCP localport=8866 remoteip=localsubnet profile=any', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec('netsh.exe', 'advfirewall firewall add rule name="Direct Tuner Recorder" dir=in action=allow ' +
      'protocol=UDP localport=8866 remoteip=localsubnet profile=any', '', SW_HIDE, ewWaitUntilTerminated, Code);

    // Let the recorder's wake timer wake the computer from sleep.
    Exec('powercfg.exe', '/SETACVALUEINDEX SCHEME_CURRENT SUB_SLEEP RTCWAKE 1', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec('powercfg.exe', '/SETDCVALUEINDEX SCHEME_CURRENT SUB_SLEEP RTCWAKE 1', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec('powercfg.exe', '/SETACTIVE SCHEME_CURRENT', '', SW_HIDE, ewWaitUntilTerminated, Code);

    if IsNetworkFolder() then
    begin
      Account := Trim(AccountPage.Values[0]);
      // The service account writes its timers and log here.
      Exec('icacls.exe', '"' + DataFolder() + '" /grant "' + Account + ':(OI)(CI)M"', '', SW_HIDE,
        ewWaitUntilTerminated, Code);
      Exec(ExpandConstant('{app}\DirectTunerRecorder.exe'),
        '--install --account "' + Account + '" --password "' + AccountPage.Values[1] + '"', '', SW_HIDE,
        ewWaitUntilTerminated, Code);
    end
    else
      Exec(ExpandConstant('{app}\DirectTunerRecorder.exe'), '--install', '', SW_HIDE, ewWaitUntilTerminated, Code);

    if Code <> 0 then
      MsgBox('The recorder was installed but didn''t start. Check the account name and password, or see ' +
        'recorder.log in ' + DataFolder() + '.', mbError, MB_OK);
  end;
end;
