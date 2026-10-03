; SPDX-License-Identifier: AGPL-3.0-or-later
; Copyright (C) 2026 Sean Snaider

; Amp Sim's Windows installer (Inno Setup 6, https://jrsoftware.org/isinfo.php). Built by the Windows CI
; workflow (.github/workflows/windows.yml):
;
;   ISCC.exe /DAppVersion=0.1.1 /DSourceDir=<folder with Amp Sim.exe> /DOutputDir=<dist> installer\windows\AmpSim.iss
;
; Decisions (docs/ASSUMPTIONS.md, Distribution):
;  - Per-user install (PrivilegesRequired=lowest), into %LOCALAPPDATA%\Programs\Amp Sim. No admin rights
;    and no UAC prompt, for the first install or for any update WinSparkle runs later.
;  - Updates: WinSparkle runs this same installer with
;      /SILENT /SP- /NOCANCEL /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1
;    (make_appcast.py). Silent shows only a small progress window; CloseApplications closes a running
;    Amp Sim first; /RELAUNCH=1 opens it again at the end.
;  - Uninstall from Settings > Apps, or the Start menu entry. User data (presets, captures, IRs, in
;    %APPDATA%\AmpSim) is left alone.
;
; UNTESTED: never compiled or run (no Windows machine here).

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build-release\AmpSim_artefacts\Release\Standalone"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
; The AppId identifies Amp Sim to Windows across versions (so an update replaces the old one). Never change it.
AppId={{03C1B276-8DA4-4D7A-A09D-FA598A60FFD3}
AppName=Amp Sim
AppVersion={#AppVersion}
AppVerName=Amp Sim {#AppVersion}
AppPublisher=Sean Snaider
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Amp Sim
DefaultGroupName=Amp Sim
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=AmpSim-{#AppVersion}-windows-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Amp Sim.exe
UninstallDisplayName=Amp Sim
CloseApplications=force
RestartApplications=no
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; The bundled content is replaced as a whole, so a file dropped from the starter pack doesn't linger.
Type: filesandordirs; Name: "{app}\content"

[Files]
Source: "{#SourceDir}\Amp Sim.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinSparkle.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\content\*"; DestDir: "{app}\content"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "Read me first.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\Amp Sim"; Filename: "{app}\Amp Sim.exe"
Name: "{autodesktop}\Amp Sim"; Filename: "{app}\Amp Sim.exe"; Tasks: desktopicon

[Run]
; After a normal (interactive) install: the usual "Launch Amp Sim" checkbox.
Filename: "{app}\Amp Sim.exe"; Description: "{cm:LaunchProgram,Amp Sim}"; Flags: nowait postinstall skipifsilent
; After a silent update from WinSparkle: reopen the app it closed.
Filename: "{app}\Amp Sim.exe"; Flags: nowait; Check: IsRelaunch

[Code]
function IsRelaunch: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;
