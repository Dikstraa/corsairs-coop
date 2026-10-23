@echo off
rem Builds modules\CoopNet.dll (x64, VC140 runtime like the stock modules).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\coopnet_vsdir.txt" || exit /b 1
set /p VSDIR=<"%TEMP%\coopnet_vsdir.txt"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /MD /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS src\coopnet.cpp src\udp_session.cpp src\coop_clock.cpp /Fobuild\ /Fe:build\CoopNet.dll /link /NOLOGO user32.lib || exit /b 1
copy /y build\CoopNet.dll ..\modules\CoopNet.dll >nul && echo OK: modules\CoopNet.dll
