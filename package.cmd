@echo off
rem ---------------------------------------------------------------------------
rem Package: Release build -> windeployqt runtime -> Inno Setup installer
rem Output : dist\PDFBoard-<version>-setup.exe
rem
rem This file is deliberately ASCII-only: cmd.exe parses batch files using the
rem active code page, and non-ASCII bytes break the parser.
rem
rem Dependencies (edit the paths below if yours differ):
rem   Visual Studio 2022 / Qt 6.8 / CMake / Ninja / Inno Setup 6
rem ---------------------------------------------------------------------------
setlocal

set "VS=D:\Microsoft Visual Studio\2022\Community"
set "QT=D:\Qt\6.8.3\msvc2022_64"
set "ISCC=D:\dev\tools\InnoSetup\ISCC.exe"

rem Keep temp on D: so the system drive is not touched.
set "TEMP=D:\dev\tmp"
set "TMP=D:\dev\tmp"

if not exist "%ISCC%" (
    echo [package] ISCC.exe not found: %ISCC%
    echo [package] Install Inno Setup 6 or fix the path in this script.
    exit /b 1
)

echo [package] 1/3 building Release...
call "%~dp0build.cmd" || ( echo [package] build failed & exit /b 1 )

echo [package] 2/3 collecting runtime...
if exist "%~dp0dist\stage" rmdir /s /q "%~dp0dist\stage"
mkdir "%~dp0dist\stage"
copy /y "%~dp0build\pdfboard.exe" "%~dp0dist\stage\" >nul || ( echo [package] copy failed & exit /b 1 )
"%QT%\bin\windeployqt.exe" --release --no-system-d3d-compiler --compiler-runtime "%~dp0dist\stage\pdfboard.exe" || ( echo [package] windeployqt failed & exit /b 1 )

echo [package] 3/3 building installer...
"%ISCC%" "%~dp0installer\pdfboard.iss" || ( echo [package] Inno Setup failed & exit /b 1 )

echo [package] OK  -^>  %~dp0dist
exit /b 0
