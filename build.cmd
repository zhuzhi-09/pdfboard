@echo off
setlocal
chcp 65001 >nul

set "VS=D:\Microsoft Visual Studio\2022\Community"
set "QT=D:\Qt\6.8.3\msvc2022_64"
set "CMAKE=D:\cmake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

rem keep temp on D: (C: is nearly full)
set "TEMP=D:\dev\tmp"
set "TMP=D:\dev\tmp"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

if not exist build mkdir build

"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT%" ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%"
if errorlevel 1 ( echo [build] configure failed & exit /b 1 )

"%CMAKE%" --build "%~dp0build"
if errorlevel 1 ( echo [build] compile failed & exit /b 1 )

echo [build] OK
exit /b 0
