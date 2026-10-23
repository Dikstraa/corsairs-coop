@echo off
rem Builds and runs the transport tests (no game needed).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\coopnet_vsdir.txt" || exit /b 1
set /p VSDIR=<"%TEMP%\coopnet_vsdir.txt"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\tests mkdir build\tests
cl /nologo /O2 /MD /EHsc /std:c++17 /W3 tests\session_test.cpp src\udp_session.cpp /Fobuild\tests\ /Fe:build\tests\session_test.exe /link /NOLOGO || exit /b 1
build\tests\session_test.exe
