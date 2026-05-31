; Inno Setup script for Provision LLM Engine -- Windows port of docker-dual.
; Bundles llama-server (CUDA+Vulkan), all runtime DLLs, models.ini and the
; two GGUF models into a single installer.  Compile with:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" llama-dual.iss
; Requires the payload\ folder produced by build-payload.ps1.

; Define "Trial" on the ISCC command line to brand a request-limited build:
;   ISCC.exe /DTrial llama-dual.iss
; The payload must have been produced by:  build-payload.ps1 -Trial
#ifdef Trial
  #define MyAppName      "Provision LLM Engine (Trial)"
  #define OutBaseName    "ProvisionLLMEngine-Trial-Setup"
  ; Distinct AppId so a trial installs side-by-side with / independently of
  ; the full version instead of silently "upgrading" it.
  #define AppGuid        "{9A1B7C42-5E3D-4F18-A2C6-7B9D0E4A1F35}"
#else
  #define MyAppName      "Provision LLM Engine"
  #define OutBaseName    "ProvisionLLMEngine-Setup"
  #define AppGuid        "{8F3C6A21-7B4D-4E92-9C1A-2D5E7A9B0C13}"
#endif
#define MyAppVersion     "1.0.0"
#define MyAppPublisher   "Provision"
#define MyAppExeName     "ProvisionLLM_launcher.bat"

[Setup]
; A fresh GUID identifies this app for upgrades/uninstall. Keep it stable
; across versions so installs upgrade in place instead of stacking.
AppId={{#AppGuid}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Models live in Program Files -> needs elevation.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename={#OutBaseName}
SetupIconFile=favicon.ico
; The bundled models push the total well past the ~4.2 GB hard limit for a
; single Setup.exe, so split into a Setup.exe + Setup-N.bin slices. All
; slices must stay in the same folder; running Setup.exe installs
; everything (models included). 2.1 GB is Inno's max slice size.
DiskSpanning=yes
DiskSliceSize=2100000000
; LZMA2 for the small files; the .gguf entries below are marked
; nocompression (already-quantised weights barely shrink and would make
; compile take forever). Solid off so nocompression actually applies.
Compression=lzma2/normal
SolidCompression=no
WizardStyle=modern
; ~7 GB of models -> make the space requirement honest in the UI.
DirExistsWarning=no
UninstallDisplayName={#MyAppName}

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything except the models: normal compression.
Source: "payload\*"; DestDir: "{app}"; Excludes: "models\*.gguf"; \
    Flags: ignoreversion recursesubdirs createallsubdirs
; Models: store without recompression (huge, already compressed).
Source: "payload\models\*.gguf"; DestDir: "{app}\models"; \
    Flags: ignoreversion nocompression

[Icons]
; Start-menu shortcut to the launcher (opens a console window with logs).
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; \
    WorkingDir: "{app}"; IconFilename: "{app}\favicon.ico"
; Start-menu shortcut that opens the WebUI in the default browser.
Name: "{group}\{#MyAppName} WebUI"; Filename: "{app}\Open-WebUI.url"
; Uninstaller entry.
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
; Optional desktop icon.
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; \
    WorkingDir: "{app}"; IconFilename: "{app}\favicon.ico"; \
    Tasks: desktopicon

[Run]
; Offer to start the server right after install.
Filename: "{app}\{#MyAppExeName}"; Description: "Start {#MyAppName} now"; \
    WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Remove the build dir and any logs the server may have written; leave
; nothing behind under {app}.
Type: filesandordirs; Name: "{app}\models"
Type: dirifempty; Name: "{app}"

[Code]
function InitializeSetup(): Boolean;
var
  DriverPresent: Boolean;
begin
  // nvcuda.dll ships with the NVIDIA driver, not with us.  Warn early if
  // it's missing rather than letting llama-server fall back to CPU silently.
  DriverPresent := FileExists(ExpandConstant('{sys}\nvcuda.dll'));
  if not DriverPresent then
  begin
    if MsgBox('NVIDIA driver (nvcuda.dll) was not found.' + #13#10 +
              'LlamaDual needs a recent NVIDIA driver for GPU acceleration.' + #13#10 +
              'You can continue, but it will run on CPU only (very slow for 7B).' + #13#10#13#10 +
              'Continue anyway?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
      Exit;
    end;
  end;
  Result := True;
end;
