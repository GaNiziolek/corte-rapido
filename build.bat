@echo off
rem Compila CorteRapido.exe (x64, CRT estatico, sem dependencias externas).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo Visual Studio com C++ nao encontrado. & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
if not exist src\app.ico powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1 || exit /b 1

rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl /nologo /utf-8 /std:c++17 /O1 /GL /MT /EHsc /W3 /DNDEBUG /Fo"build\\" src\main.cpp build\app.res ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /OUT:CorteRapido.exe || exit /b 1
echo.
echo OK: %~dp0CorteRapido.exe
