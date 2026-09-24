; ---------------------------------------------------------------------------
; 落墨·大屏批注 (PDFBoard) - Inno Setup script
;
; 打包：tools\package.cmd（先生成 dist\stage，再调用本脚本）
; 产物：dist\PDFBoard-<版本>-setup.exe
;
; 静默安装（供教室集中管理客户端调用）：
;   PDFBoard-1.7.3-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
;   PDFBoard-1.7.3-setup.exe /VERYSILENT /NORESTART /TASKS=""      ; 不建桌面快捷方式
; ---------------------------------------------------------------------------

#define AppName      "落墨·大屏批注"
#define AppPublisher "PDFBoard"
#define AppExe       "pdfboard.exe"
#define StageDir     "..\dist\stage"

; These two can be overridden from the command line - the nightly workflow
; passes /DAppVersion=... /DOutputBase=... so its artifacts do not collide with
; the tagged releases. Local builds keep the plain version below.
#ifndef AppVersion
  #define AppVersion "1.11.3"
#endif
#ifndef OutputBase
  #define OutputBase "PDFBoard-" + AppVersion + "-setup"
#endif

[Setup]
; AppId 必须在所有版本间保持不变，否则升级会装成两份。
AppId={{9C1F4E2A-7B3D-4E58-9A21-6D5C0B8F3A71}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\PDFBoard
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\dist
OutputBaseFilename={#OutputBase}
SetupIconFile=..\assets\pdfboard.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 默认按当前用户安装（不弹 UAC，适合静默下发）；手动运行时可在向导里改为"为所有用户"。
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 any build. Qt 6.8's *tested* baseline is 1809, but the binaries usually run on
; older builds too - refusing to install there would lock out schools we already support, so
; the gate stays at "Windows 10" and older builds get a clear message at runtime instead.
MinVersion=10.0
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes

[Languages]
Name: "cn"; MessagesFile: "compiler:Default.isl,ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; --- .pdf 关联（当前用户；与程序设置页里的"设为默认打开方式"一致） ---
Root: HKCU; Subkey: "Software\Classes\PDFBoard.Pdf"; ValueType: string; ValueName: ""; ValueData: "PDF 文档"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\PDFBoard.Pdf\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKCU; Subkey: "Software\Classes\PDFBoard.Pdf\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: none; ValueName: "PDFBoard.Pdf"; Flags: uninsdeletevalue
; --- .dpz 批注包关联 ---
Root: HKCU; Subkey: "Software\Classes\PDFBoard.Dpz"; ValueType: string; ValueName: ""; ValueData: "落墨批注包"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\PDFBoard.Dpz\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKCU; Subkey: "Software\Classes\PDFBoard.Dpz\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\.dpz\OpenWithProgids"; ValueType: none; ValueName: "PDFBoard.Dpz"; Flags: uninsdeletevalue
; --- 让程序出现在系统「默认应用」列表里 ---
Root: HKCU; Subkey: "Software\PDFBoard\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#AppName}"
Root: HKCU; Subkey: "Software\PDFBoard\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "大屏 PDF 查看与批注工具"
Root: HKCU; Subkey: "Software\PDFBoard\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pdf"; ValueData: "PDFBoard.Pdf"
Root: HKCU; Subkey: "Software\PDFBoard\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dpz"; ValueData: "PDFBoard.Dpz"
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "PDFBoard"; ValueData: "Software\PDFBoard\Capabilities"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExe}"; Description: "立即运行 {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 卸载时清掉从批注包里解出来的临时源文件
Type: filesandordirs; Name: "{tmp}\pdfboard"
