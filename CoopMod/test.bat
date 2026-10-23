@echo off
rem Builds and runs the engine-independent tests (no game needed).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\coopnet_vsdir.txt" || exit /b 1
set /p VSDIR=<"%TEMP%\coopnet_vsdir.txt"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\tests mkdir build\tests
set CL_FLAGS=/nologo /O2 /MD /EHsc /std:c++17 /W3 /Fobuild\tests\
cl %CL_FLAGS% tests\clock_test.cpp src\coop_clock.cpp /Fe:build\tests\clock_test.exe /link /NOLOGO || exit /b 1
cl %CL_FLAGS% tests\vote_test.cpp src\coop_vote.cpp /Fe:build\tests\vote_test.exe /link /NOLOGO || exit /b 1
cl %CL_FLAGS% tests\session_test.cpp src\udp_session.cpp /Fe:build\tests\session_test.exe /link /NOLOGO || exit /b 1
echo ===== clock_test
build\tests\clock_test.exe || exit /b 1
echo ===== vote_test
build\tests\vote_test.exe || exit /b 1
echo ===== session_test
build\tests\session_test.exe
