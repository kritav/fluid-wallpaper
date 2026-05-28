; Inno Setup script for Fluid Wallpaper.
; Build with:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\fluid-wallpaper.iss
; Produces installer\output\fluid-wallpaper-setup-<version>.exe.

#define MyAppName       "Fluid Wallpaper"
#define MyAppVersion    "0.1.0"
#define MyAppPublisher  "kritav"
#define MyAppURL        "https://github.com/kritav/fluid-wallpaper"
#define MyAppExeName    "fluid-wallpaper.exe"

[Setup]
; AppId pins the installer's identity across versions. Never change this once
; released or Windows will treat upgrades as separate installs.
AppId={{8F4A3B7C-2E5D-4A1F-9C3B-7E8D2F1A0B5C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases

DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=fluid-wallpaper-setup-{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

SetupIconFile=..\wallpaper\resources\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

; Per-user by default; admin elevation only if the user explicitly picks
; "Install for all users" in the wizard.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart";   Description: "Start Fluid Wallpaper automatically when Windows starts"; GroupDescription: "Additional options:"
Name: "desktopicon"; Description: "Create a desktop shortcut";                              GroupDescription: "Additional options:"; Flags: unchecked

[Files]
; The exe path assumes a CMake "Release" multi-config build. If your build
; output sits elsewhere (e.g. single-config: build\fluid-wallpaper.exe), edit
; this line.
Source: "..\wallpaper\build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; Shaders are loaded at runtime by D3DCompile, so they have to ship.
Source: "..\wallpaper\build\Release\shaders\*";       DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs
Source: "..\README.md";                                DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                                  DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";          Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}";    Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Autostart entry. The tray menu's "Start with Windows" toggle writes/removes
; the exact same value, so the two are interchangeable.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "fluid-wallpaper"; \
    ValueData: """{app}\{#MyAppExeName}"""; \
    Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; \
    Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; \
    Flags: nowait postinstall skipifsilent

[Code]
function InitializeSetup(): Boolean;
begin
    // nvcuda.dll ships with the NVIDIA driver; absence means CUDA can't load.
    // Don't hard-block (the check can be wrong on some configs), just warn.
    if not FileExists(ExpandConstant('{sys}\nvcuda.dll')) then
    begin
        if MsgBox('Fluid Wallpaper requires an NVIDIA GPU with CUDA support.'
                  + #13#10
                  + 'CUDA does not appear to be installed on this system.'
                  + #13#10 + #13#10
                  + 'Continue installation anyway?',
                  mbConfirmation, MB_YESNO) = IDNO then
        begin
            Result := False;
            Exit;
        end;
    end;
    Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
    if CurUninstallStep = usPostUninstall then
    begin
        // App settings (incl. first-run flag). Autostart entry already
        // removed via uninsdeletevalue above.
        RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER,
            'Software\kritav\FluidWallpaper');
    end;
end;
