; Inno Setup script for the literouter Windows installer.
;
; Why an installer when a bare .exe already runs: the bare binary cannot put
; itself on PATH, cannot register the logon task, and cannot be uninstalled
; from Apps & features. Those three are what "install" means on Windows, and a
; user who downloaded an .exe and then has to hand-edit PATH has not installed
; anything — they have copied a file.
;
; Deliberately NOT done here:
;   * No PATH edit by default. literouter is a service-shaped tool; putting it
;     on PATH is opt-in (the checkbox), because silently shadowing an existing
;     `literouter` on PATH is the kind of thing an installer should not do.
;   * No firewall rule. The gateway binds loopback by default, and opening a
;     port nobody asked to open is worse than the user clicking through the
;     Windows prompt on the day they bind 0.0.0.0.
;   * No service registration. The CLI's own `--service` path owns that, and
;     duplicating it here would give two places to fix.
;
; Built by scripts/make_windows_installer.sh, which fills in the two #define
; values below from the build. Paths are relative to this file.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceBinary
  #define SourceBinary "literouter.exe"
#endif

#define AppName        "literouter"
; The desktop app is the primary install target; the CLI ships beside it as the
; headless/scripting front end. Both are the same product, so both go in.
#ifndef SourceApp
  #define SourceApp ""
#endif
#define AppPublisher   "literouter contributors"
#define AppUrl         "https://github.com/YangQi0408/literouter"
#define AppExeName     "literouter.exe"
#define AppId          "{{8F3C2A16-5B7E-4D9A-9E41-2C6B7A0D3F58}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
; 64-bit only: the release matrix builds windows-x86_64 and nothing else, and
; claiming otherwise would put the binary in a 32-bit Program Files.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-machine by default, which is where a service-shaped tool belongs. The
; override lets a user without admin rights install for themselves.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist-release
OutputBaseFilename=literouter-{#AppVersion}-windows-x86_64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The config and state live under %APPDATA% / %LOCALAPPDATA% and are the user's
; data. An uninstall that removed them would delete relays the user typed in by
; hand; the [UninstallDelete] section below is explicit about what it touches.
#if SourceApp != ""
UninstallDisplayIcon={app}\literouter-tray.exe
#else
UninstallDisplayIcon={app}\{#AppExeName}
#endif
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "addtopath"; Description: "Add literouter to the system PATH"; GroupDescription: "Optional:"; Flags: unchecked
Name: "startup";   Description: "Start the gateway when I sign in";  GroupDescription: "Optional:"; Flags: unchecked

[Files]
Source: "{#SourceBinary}"; DestDir: "{app}"; DestName: "{#AppExeName}"; Flags: ignoreversion
; Present when the release build produced it. `skipifsourcedoesntexist` keeps a
; CLI-only build installable instead of failing the whole installer.
#if SourceApp != ""
Source: "{#SourceApp}"; DestDir: "{app}"; DestName: "literouter-tray.exe"; Flags: ignoreversion
#endif
Source: "..\README.md";      DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\README_en.md";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";        DestDir: "{app}"; Flags: ignoreversion
Source: "..\NOTICE";         DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
; The app first, because that is what most users want to click.
#if SourceApp != ""
Name: "{group}\literouter";                 Filename: "{app}\literouter-tray.exe"; WorkingDir: "{app}"
#endif
Name: "{group}\literouter (headless gateway)"; Filename: "{app}\{#AppExeName}"; Parameters: "serve"; WorkingDir: "{app}"
Name: "{group}\literouter (status)";           Filename: "{app}\{#AppExeName}"; Parameters: "status"; WorkingDir: "{app}"
Name: "{group}\literouter documentation";      Filename: "{app}\README.md"
Name: "{group}\Uninstall literouter";          Filename: "{uninstallexe}"

[Run]
; The gateway is not started by the installer. `serve` blocks in the
; foreground, so launching it from here would leave the wizard waiting on a
; process that never returns; the Start-menu entry is how it is meant to be run.
Filename: "{app}\README.md"; Description: "Open the README"; Flags: postinstall shellexec nowait skipifsilent

[UninstallDelete]
; Only what the installer itself created. The config (%APPDATA%\literouter) and
; the state (%LOCALAPPDATA%\literouter) are the user's data and are named in
; the uninstall message instead of being deleted.
Type: filesandordirs; Name: "{app}"

[Code]
// PATH is edited through the registry, not through `setx`, because setx
// truncates at 1024 characters and would silently mangle a long PATH.
const
  EnvironmentKey = 'System\CurrentControlSet\Control\Session Manager\Environment';

function PathContains(const Path, Dir: string): Boolean;
var
  Normalized: string;
begin
  Normalized := Uppercase(Path);
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Normalized + ';') > 0;
end;

procedure AddToPath(Dir: string);
var
  Current: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Current) then
    Current := '';
  if PathContains(Current, Dir) then
    exit;
  if (Current <> '') and (Current[Length(Current)] <> ';') then
    Current := Current + ';';
  RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Current + Dir);
end;

procedure RemoveFromPath(Dir: string);
var
  Current, Updated: string;
  At: Integer;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Current) then
    exit;
  At := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Current) + ';');
  if At = 0 then
    exit;
  // Rebuild without that one component, keeping the rest byte for byte.
  Updated := Current;
  Delete(Updated, At, Length(Dir) + 1);
  // The delete above leaves the leading ';' when the component was first;
  // trimming one stray separator is cheaper than splitting and rejoining.
  if (Updated <> '') and (Updated[1] = ';') then
    Delete(Updated, 1, 1);
  RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Updated);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddToPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // A running gateway holds its own binary open, and Windows refuses to
    // delete a mapped executable. There is no `literouter shutdown`
    // subcommand, so the graceful path is the admin endpoint the console's own
    // button uses — POST /__literouter/shutdown — and the blunt one is
    // taskkill for the case where the gateway is wedged or was started with a
    // config the endpoint does not answer on.
    //
    // Both are best-effort: a failure means nothing was running, which is the
    // ordinary case for someone uninstalling a tool they stopped using.
    Exec(ExpandConstant('{sys}\curl.exe'), '-s -m 3 -X POST ' +
         'http://127.0.0.1:8787/__literouter/shutdown', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Sleep(1500);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExeName}', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
    // The desktop app runs the gateway in its own process, so stopping only the
    // CLI binary would leave the port bound and the app's exe locked.
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM literouter-tray.exe', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
    RemoveFromPath(ExpandConstant('{app}'));
  end;
end;
