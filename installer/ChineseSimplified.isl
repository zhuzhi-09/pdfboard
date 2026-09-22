; ---------------------------------------------------------------------------
; 简体中文（局部覆盖）
;
; 只覆盖安装向导里最常出现的文案；未列出的条目会自动使用 Default.isl（英文），
; 因此这个文件可以保持很小，也不会因为 Inno Setup 升级而失效。
; 编码：GBK（与下面的 LanguageCodePage=936 对应）。
; ---------------------------------------------------------------------------
[LangOptions]
LanguageName=简体中文
LanguageID=$0804
LanguageCodePage=936

[Messages]
SetupAppTitle=安装
SetupWindowTitle=安装 - %1
WelcomeLabel1=欢迎使用 %1 安装向导
WelcomeLabel2=即将在你的电脑上安装 %1。%n%n点击「下一步」继续。
WizardSelectDir=选择安装位置
SelectDirLabel3=安装程序将把 %1 安装到下面的文件夹。
SelectDirBrowseLabel=点击「浏览」选择其它文件夹；若直接点击「下一步」则使用上面的文件夹。
SelectDirNeedAdmin=安装到该位置需要管理员权限。
WizardSelectTasks=选择附加任务
SelectTasksLabel2=请选择安装程序要一并完成的附加任务，然后点击「下一步」。
WizardReady=准备安装
ReadyLabel1=安装程序已准备好，即将开始在电脑上安装 %1。
ReadyLabel2a=点击「安装」开始安装；如需检查或修改设置，点击「上一步」。
ReadyLabel2b=点击「安装」开始安装。
PreparingDesc=安装程序正在准备安装 %1，请稍候。
InstallingLabel=正在安装 %1，请稍候…
FinishedHeadingLabel=安装完成
FinishedLabel=安装完成。%n%n双击桌面图标即可启动 %1。
FinishedLabelNoIcons=%1 安装完成。
ClickFinish=点击「完成」结束安装。
ButtonNext=下一步(&N) > 
ButtonBack=< 上一步(&B)
ButtonInstall=安装(&I)
ButtonFinish=完成(&F)
ButtonCancel=取消
ButtonBrowse=浏览(&R)…
ButtonYes=是(&Y)
ButtonNo=否(&N)
ButtonOK=确定
ButtonClose=关闭
ExitSetupTitle=退出安装
ExitSetupMessage=安装尚未完成，确定要退出吗？
UninstallAppFullTitle=卸载 %1
ConfirmUninstall=确定要卸载 %1 及其全部组件吗？
UninstalledAll=%1 已成功卸载。
UninstallStatusLabel=正在卸载 %1，请稍候…
UninstallUninstalling=正在移除文件与设置…
ErrorChangingAttr=无法修改文件属性：%1
ErrorCopying=复制文件失败：%1
