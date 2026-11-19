; FluxDM installer (Inno Setup 6, as FDM uses). Built by scripts\package.ps1 -Installer:
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=<staged portable folder> /DOutputDir=<dist> installer\FluxDM.iss
; Per-user by default (no administrator rights needed); "for all users" is offered too.

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
DefaultDirName={autopf}\FluxDM
DefaultGroupName=FluxDM
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=FluxDM-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\FluxDM.exe
; Close a running FluxDM (and its browser helper) before replacing files; start it again afterwards.
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\FluxDM"; Filename: "{app}\FluxDM.exe"
Name: "{autodesktop}\FluxDM"; Filename: "{app}\FluxDM.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\FluxDM.exe"; Description: "{cm:LaunchProgram,FluxDM}"; Flags: nowait postinstall skipifsilent
; An update started from FluxDM runs silently: start FluxDM again when it's done.
Filename: "{app}\FluxDM.exe"; Flags: nowait; Check: WizardSilent

[UninstallRun]
; Removes the browser helper registration and magnet/.torrent associations of this Windows account.
; Downloads, the download list and settings in %LOCALAPPDATA%\FluxDM are kept.
Filename: "{app}\FluxDM.exe"; Parameters: "--uninstall-cleanup"; Flags: runhidden waituntilterminated; RunOnceId: "FluxDMCleanup"
