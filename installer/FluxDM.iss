; FluxDM installer (Inno Setup 6, as FDM uses). Built by scripts\package.ps1 -Installer:
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=<staged portable folder> /DOutputDir=<dist> installer\FluxDM.iss
; Per-user by default (no administrator rights needed); "for all users" is offered too.
; Silent install, e.g. for a test or a deployment script:
;   FluxDM-<ver>-setup.exe /VERYSILENT /CURRENTUSER /DIR=<folder> /TASKS="assoc,browser"

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error SourceDir must point at the staged files (scripts\package.ps1 does this)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Never change AppId: it is how upgrades and the uninstaller find an existing installation.
AppId={{6D3B1A52-7C1E-4E8F-9A65-2F0C8B7D4E11}
AppName=FluxDM
AppVersion={#AppVersion}
AppVerName=FluxDM {#AppVersion}
AppPublisher=FluxDM
VersionInfoVersion={#AppVersion}
VersionInfoDescription=FluxDM Setup
DefaultDirName={autopf}\FluxDM
DefaultGroupName=FluxDM
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=FluxDM-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#SourcePath}\..\resources\fluxdm.ico
UninstallDisplayIcon={app}\FluxDM.exe
UninstallDisplayName=FluxDM
; Third-party programs shipped in tools\ (ffmpeg is GPL): shown before installing.
#if FileExists(SourceDir + "\tools\README.txt")
InfoBeforeFile={#SourceDir}\tools\README.txt
#endif
; Close a running FluxDM (and its browser helper) before replacing files; start it again afterwards.
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assoc"; Description: "Open .torrent files and magnet links with FluxDM"; GroupDescription: "Integration:"
Name: "browser"; Description: "Connect FluxDM to Chrome, Edge, Brave and Firefox (then load the extension: Settings > Browser)"; GroupDescription: "Integration:"
Name: "autostart"; Description: "Start FluxDM with Windows (in the tray)"; GroupDescription: "Integration:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\FluxDM"; Filename: "{app}\FluxDM.exe"
Name: "{autodesktop}\FluxDM"; Filename: "{app}\FluxDM.exe"; Tasks: desktopicon

[Run]
; The ticked integration options, done by FluxDM itself (the same code as its Settings switches),
; for the Windows account that runs Setup, also when Setup was elevated "for all users".
; Not on a silent update from inside FluxDM: what the user changed in Settings since then stays.
Filename: "{app}\FluxDM.exe"; Parameters: "--install-setup assoc"; Tasks: assoc; Flags: runhidden waituntilterminated runasoriginaluser; Check: ApplyIntegration
Filename: "{app}\FluxDM.exe"; Parameters: "--install-setup browser"; Tasks: browser; Flags: runhidden waituntilterminated runasoriginaluser; Check: ApplyIntegration
Filename: "{app}\FluxDM.exe"; Parameters: "--install-setup autostart"; Tasks: autostart; Flags: runhidden waituntilterminated runasoriginaluser; Check: ApplyIntegration
Filename: "{app}\FluxDM.exe"; Description: "{cm:LaunchProgram,FluxDM}"; Flags: nowait postinstall skipifsilent runasoriginaluser
; An update started from FluxDM runs silently: start FluxDM again when it's done.
Filename: "{app}\FluxDM.exe"; Flags: nowait runasoriginaluser; Check: IsSilentUpdate

[UninstallRun]
; Removes the browser helper registration, magnet/.torrent associations and "start with Windows"
; of this Windows account.
Filename: "{app}\FluxDM.exe"; Parameters: "--uninstall-cleanup"; Flags: runhidden waituntilterminated; RunOnceId: "FluxDMCleanup"

[Code]
var
  WasInstalled: Boolean;

function InitializeSetup(): Boolean;
var
  Key: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6D3B1A52-7C1E-4E8F-9A65-2F0C8B7D4E11}_is1';
  WasInstalled := RegKeyExists(HKCU, Key) or RegKeyExists(HKLM, Key);
  Result := True;
end;

// A silent run over an existing installation: an update started from inside FluxDM.
function IsSilentUpdate(): Boolean;
begin
  Result := WizardSilent and WasInstalled;
end;

function ApplyIntegration(): Boolean;
begin
  Result := not IsSilentUpdate;
end;

// Downloads, the download list and settings (%LOCALAPPDATA%\FluxDM) are kept unless the user
// asks for them to go too. Never asked in a silent uninstall.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
  begin
    Data := ExpandConstant('{localappdata}\FluxDM');
    if DirExists(Data) and
       (MsgBox('Also delete your FluxDM download list and settings?' + #13#10#13#10 +
               'Downloaded files are not affected. Choose No to keep them for a later installation.',
               mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(Data, True, True, True);
  end;
end;
