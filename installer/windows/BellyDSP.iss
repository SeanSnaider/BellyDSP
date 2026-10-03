; SPDX-License-Identifier: AGPL-3.0-or-later
; Copyright (C) 2026 Sean Snaider

; BellyDSP's Windows installer (Inno Setup 6, https://jrsoftware.org/isinfo.php). Built by the Windows CI
; workflow (.github/workflows/windows.yml):
;
;   ISCC.exe /DAppVersion=0.1.1 /DSourceDir=<folder with BellyDSP.exe> /DOutputDir=<dist> installer\windows\BellyDSP.iss
;
; Decisions (docs/ASSUMPTIONS.md, Distribution):
;  - Per-user install (PrivilegesRequired=lowest), into %LOCALAPPDATA%\Programs\BellyDSP. No admin rights
;    and no UAC prompt, for the first install or for any update WinSparkle runs later.
;  - Updates: WinSparkle runs this same installer with
;      /SILENT /SP- /NOCANCEL /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1
;    (make_appcast.py). Silent shows only a small progress window; CloseApplications closes a running
;    BellyDSP first; /RELAUNCH=1 opens it again at the end.
;  - Uninstall from Settings > Apps, or the Start menu entry. User data (presets, captures, IRs, in
;    %APPDATA%\BellyDSP) is left alone.
;
; UNTESTED: never compiled or run (no Windows machine here).

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build-release\BellyDSP_artefacts\Release\Standalone"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
; The AppId identifies BellyDSP to Windows across versions (so an update replaces the old one). Never change it
; once a release has shipped. (New on 2026-10-03 with the rename; the "Amp Sim" one never shipped.)
AppId={{6C67B769-28E0-42DB-9564-F057142EE769}
AppName=BellyDSP
AppVersion={#AppVersion}
AppVerName=BellyDSP {#AppVersion}
AppPublisher=Sean Snaider
AppPublisherURL=https://github.com/SeanSnaider/BellyDSP
AppCopyright=Copyright (C) 2026 Sean Snaider, GNU AGPL v3 or later
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\BellyDSP
DefaultGroupName=BellyDSP
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=BellyDSP-{#AppVersion}-windows-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\BellyDSP.exe
UninstallDisplayName=BellyDSP
CloseApplications=force
RestartApplications=no
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; The bundled content is replaced as a whole, so a file dropped from the starter pack doesn't linger.
Type: filesandordirs; Name: "{app}\content"

[Files]
Source: "{#SourceDir}\BellyDSP.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinSparkle.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\content\*"; DestDir: "{app}\content"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "Read me first.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\BellyDSP"; Filename: "{app}\BellyDSP.exe"
Name: "{autodesktop}\BellyDSP"; Filename: "{app}\BellyDSP.exe"; Tasks: desktopicon

[Run]
; After a normal (interactive) install: the usual "Launch BellyDSP" checkbox.
Filename: "{app}\BellyDSP.exe"; Description: "{cm:LaunchProgram,BellyDSP}"; Flags: nowait postinstall skipifsilent
; After a silent update from WinSparkle: reopen the app it closed.
Filename: "{app}\BellyDSP.exe"; Flags: nowait; Check: IsRelaunch

[Code]
function IsRelaunch: Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;
