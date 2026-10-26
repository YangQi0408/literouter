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
#ifndef ReleaseTag
  #define ReleaseTag AppVersion
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
; Per-user by default; explicitly requesting all users selects the system scope.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist-release
OutputBaseFilename=literouter-{#ReleaseTag}-windows-x86_64-setup
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
; English only, and the reason is not laziness: Inno Setup ships Default.isl
; with the compiler, but every other language is a separate download from the
; Inno Setup translations page — Chocolatey's package does not include them.
; Naming ChineseSimplified.isl here made the whole installer fail to compile on
; the release runner with "Couldn't open include file", which cost a v0.5.0
; build before it was spotted.
;
; Rather than fetch a .isl at package time (a network dependency in a release
; step, for one language), the few strings a user actually reads are given both
; forms inline below. That keeps the compiler self-contained.
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; The only task. "Start the gateway when I sign in" used to live here too, but
; autostart moved to the desktop app's own --autostart flag, and a checkbox no
; [Code] section reads is a switch that does nothing — worse than not offering
; it.
Name: "addtopath"; Description: "Add literouter to PATH / 添加到 PATH"; GroupDescription: "Optional / 可选:"; Flags: unchecked

[Files]
Source: "{#SourceBinary}"; DestDir: "{app}"; DestName: "{#AppExeName}"; Flags: ignoreversion
; Optional for a manually assembled CLI-only installer; required by the release
; workflow. When passed, a missing app fails packaging rather than being hidden.
#if SourceApp != ""
Source: "{#SourceApp}"; DestDir: "{app}"; DestName: "literouter-tray.exe"; Flags: ignoreversion
Source: "assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs
#endif
Source: "..\README.md";      DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\README_en.md";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";        DestDir: "{app}"; Flags: ignoreversion
Source: "..\NOTICE";         DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

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
; Inno removes files recorded by [Files] itself. Never recursively delete the
; application directory: untracked files there may also belong to the user.
; This marker is created only when this installation actually adds PATH.
Type: files; Name: "{app}\.literouter-path-added"

[Code]
// PATH is edited through the registry, not through `setx`, because setx
// truncates at 1024 characters and would silently mangle a long PATH.
const
  EnvironmentKey = 'System\CurrentControlSet\Control\Session Manager\Environment';

function EnvironmentRoot: Integer;
begin
  if IsAdminInstallMode then Result := HKEY_LOCAL_MACHINE
  else Result := HKEY_CURRENT_USER;
end;

function EnvironmentPath: string;
begin
  if IsAdminInstallMode then Result := EnvironmentKey
  else Result := 'Environment';
end;

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
  if not RegQueryStringValue(EnvironmentRoot, EnvironmentPath, 'Path', Current) then
    Current := '';
  if PathContains(Current, Dir) then
    exit;
  if (Current <> '') and (Current[Length(Current)] <> ';') then
    Current := Current + ';';
  if RegWriteExpandStringValue(EnvironmentRoot, EnvironmentPath, 'Path', Current + Dir) then
  begin
    if not SaveStringToFile(ExpandConstant('{app}\.literouter-path-added'), Dir, False) then
      MsgBox('Could not record PATH ownership. Uninstall will preserve this PATH entry.', mbInformation, MB_OK);
  end
  else
    MsgBox('Could not update PATH. Add the installation directory manually.', mbError, MB_OK);
end;

procedure RemoveFromPath(Dir: string);
var
  Current, Updated: string;
  At: Integer;
begin
  if not RegQueryStringValue(EnvironmentRoot, EnvironmentPath, 'Path', Current) then
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
  RegWriteExpandStringValue(EnvironmentRoot, EnvironmentPath, 'Path', Updated);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddToPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    // Inno Setup's Restart Manager owns files of THIS install. Never POST to a
    // guessed port or taskkill by image name: another installation can be the
    // operator's active gateway and is not this uninstaller's to stop.
    if FileExists(ExpandConstant('{app}\.literouter-path-added')) then
      RemoveFromPath(ExpandConstant('{app}'));
  end;
end;
