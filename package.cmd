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
rem No --compiler-runtime (the per-user install cannot elevate to install it),
rem no translation bundle except the Chinese one the app asks for.
"%QT%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler "%~dp0dist\stage\pdfboard.exe" || ( echo [package] windeployqt failed & exit /b 1 )
  del /q "%~dp0dist\stage\dxcompiler.dll" "%~dp0dist\stage\dxil.dll" "%~dp0dist\stage\vc_redist.x64.exe" 2>nul

  rem VC++ runtime, deployed app-local. A classroom machine may not have it installed
  rem at all (then the app cannot start: "MSVCP140.dll is missing"), and vc_redist
  rem needs admin rights, which this per-user installer deliberately avoids. Copying the
  rem DLLs next to the exe is Microsoft's supported deployment; the VS redist folder is
  rem the licensed source. Failing here is better than shipping a package that will not
  rem start on the machines we care about.
  set "CRT="
  for /d %%D in ("%VS%\VC\Redist\MSVC\*") do for /d %%C in ("%%D\x64\Microsoft.VC*.CRT") do if exist "%%C\msvcp140.dll" set "CRT=%%C"
  if not defined CRT (
      echo [package] ERROR: VC redist folder not found under "%VS%\VC\Redist\MSVC"
      exit /b 1
  )
  rem Copy only the modules the app and Qt actually import. The full CRT folder also
  rem contains brand-new helpers (vcruntime140_threads.dll, msvcp140_atomic_wait.dll)
  rem that fail to initialise on older Windows - shipping them made the app die at
  rem startup with 0xC0000142 (DLL init failed) on the build runner.
  for %%F in (msvcp140.dll msvcp140_1.dll msvcp140_2.dll vcruntime140.dll vcruntime140_1.dll) do (
      if not exist "%CRT%\%%F" ( echo [package] ERROR: missing %%F in %CRT% & exit /b 1 )
      copy /y "%CRT%\%%F" "%~dp0dist\stage\" >nul || ( echo [package] copy of %%F failed & exit /b 1 )
  )
  echo [package] VC runtime bundled from %CRT%
mkdir "%~dp0dist\stage\translations" 2>nul
copy /y "%QT%\translations\*zh_CN.qm" "%~dp0dist\stage\translations\" >nul 2>nul

echo [package] 3/3 building installer...
"%ISCC%" "%~dp0installer\pdfboard.iss" || ( echo [package] Inno Setup failed & exit /b 1 )

echo [package] OK  -^>  %~dp0dist
exit /b 0
