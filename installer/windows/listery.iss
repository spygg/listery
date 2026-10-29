; Listery Windows 安装脚本（Inno Setup 6）
; 用法：ISCC.exe installer\windows\listery.iss /DMyAppVersion=1.1.0 [/DSourceDir=dist]
; 免管理员（安装到当前用户的 Programs 目录），同时提供“开机启动”与“桌面快捷方式”可选任务。

#ifndef MyAppVersion
  #define MyAppVersion "1.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\Release"
#endif

#define MyAppName "Listery"
#define MyAppPublisher "spygg"
#define MyAppURL "https://github.com/spygg/listery"
#define MyAppExeName "listery.exe"

[Setup]
AppId={{A1F4C7E2-9B3D-4E6A-8C51-0D2F7A6B4E91}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
; 每用户安装：不需要管理员权限，也不会写 Program Files
DefaultDirName={userpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=.
OutputBaseFilename=listery-windows-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardResizable=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64
UninstallDisplayName={#MyAppName} {#MyAppVersion}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: unchecked
Name: "startup"; Description: "开机自动启动（登录后静默运行）"; GroupDescription: "附加选项:"

[Files]
; 整个发布目录（主程序 + Qt 运行库 + plugins\translator.dll）
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 开机启动用 HKCU Run 项（与程序内“开机自启动”设置等价），卸载时自动清除
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#MyAppName}"; \
    ValueData: """{app}\{#MyAppExeName}"" --autostart"; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "安装完成后启动 Listery"; \
    Flags: nowait postinstall skipifsilent unchecked
