; TombStone Engine + editor: Windows setup (Inno Setup 6).
; Built by tools/release/build_release.ps1, which stages the files and
; passes /DAppVersion=<VERSION> /DStageDir=<stage> /DOutDir=<out>.
; Per-user by default (no admin); "Install for all users" is offered on the
; first page or with /ALLUSERS.
;
; /DUpdate=1 builds TombStone-<ver>-update.exe instead: the same AppId and
; files, so it upgrades an existing install in place (same folder, tasks and
; privileges mode; Documents\TombStone with claims, settings and the Vault is
; never touched), with a minimal wizard: no welcome, licence, folder or task
; pages. The editor's update notifier prefers it (docs/updates.md).

#ifndef AppVersion
  #error AppVersion is required (from the VERSION file)
#endif
#ifndef StageDir
  #error StageDir is required
#endif
#ifndef OutDir
  #define OutDir "."
#endif

#define AppName "TombStone Engine"
#define AppPublisher "TombStone Team"
#define AppUrl "https://github.com/bjrzuchovsky-star/TombStone_Engine"

[Setup]
AppId={{6F1D3B0A-7C52-4E1B-9A3D-5B2C8E4F7A10}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
VersionInfoVersion={#AppVersion}.0
DefaultDirName={autopf}\TombStone Engine
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\ts_admin.exe
UninstallDisplayName={#AppName} {#AppVersion}
OutputDir={#OutDir}
#ifdef Update
OutputBaseFilename=TombStone-{#AppVersion}-update
#else
OutputBaseFilename=TombStone-{#AppVersion}-setup
#endif
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
SetupLogging=yes
#ifdef Update
; Upgrade in place: reuse the previous install's folder, Start menu group,
; tasks, language and privileges mode, and skip the pages that would ask.
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes
UsePreviousSetupType=yes
UsePreviousLanguage=yes
UsePreviousPrivileges=yes
DisableWelcomePage=yes
DisableDirPage=auto
DisableReadyPage=yes
; The editor closes itself after starting this; Restart Manager handles
; any other TombStone app still open.
CloseApplications=yes
RestartApplications=no
#else
LicenseFile={#StageDir}\LICENSE
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
; The editor keeps its settings (TombStoneConfig) and projects
; (TombStoneProjects) in its working folder: Documents\TombStone, which the
; uninstaller leaves alone.
Name: "{userdocs}\TombStone"; Flags: uninsneveruninstall

[Icons]
Name: "{autoprograms}\{#AppName}\TombStone Editor"; Filename: "{app}\ts_admin.exe"; WorkingDir: "{userdocs}\TombStone"; Comment: "TombStone Engine editor (ts_admin)"
Name: "{autoprograms}\{#AppName}\TombStone Client"; Filename: "{app}\ts_client.exe"; WorkingDir: "{userdocs}\TombStone"; Comment: "Multiplayer client (ts_client)"
Name: "{autoprograms}\{#AppName}\Documentation"; Filename: "{app}\docs\README.md"
Name: "{autoprograms}\{#AppName}\Samples"; Filename: "{app}\samples"
Name: "{autoprograms}\{#AppName}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TombStone Editor"; Filename: "{app}\ts_admin.exe"; WorkingDir: "{userdocs}\TombStone"; Tasks: desktopicon

[Run]
Filename: "{app}\ts_admin.exe"; WorkingDir: "{userdocs}\TombStone"; Description: "{cm:LaunchProgram,TombStone Editor}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Files the apps write next to themselves (logs); user data lives elsewhere.
Type: files; Name: "{app}\*.log"

[Code]
#ifdef Update
// An update reuses the tasks picked at install time (UsePreviousTasks), so
// the task page is skipped too.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpSelectTasks);
end;

#endif
// GPU gate: the editor and the game need OpenGL 3.3. ts_game checks itself
// too (a friendly box at start); this tells the user at install time.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssPostInstall then begin
    if Exec(ExpandConstant('{app}\ts_game.exe'), '--check-gpu --quiet', ExpandConstant('{app}'),
            SW_HIDE, ewWaitUntilTerminated, Code) then begin
      Log('GPU check exit code ' + IntToStr(Code));
      if Code <> 0 then
        SuppressibleMsgBox('TombStone Engine needs a graphics card with OpenGL 3.3 or newer, and this PC''s driver did not offer it.'
          + #13#10#13#10 + 'The engine is installed, but the editor may not start. Updating the graphics driver (NVIDIA, AMD or Intel) usually fixes this.',
          mbInformation, MB_OK, IDOK);
    end else
      Log('GPU check could not run');
  end;
end;
