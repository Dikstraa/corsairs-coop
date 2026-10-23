@echo off
rem Builds modules\CoopNet.dll (x64, VC140 runtime like the stock modules).
rem With the Steamworks SDK unpacked in steamworks_sdk\ (not in git) the Steam transport is included.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\coopnet_vsdir.txt" || exit /b 1
set /p VSDIR=<"%TEMP%\coopnet_vsdir.txt"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

set SOURCES=src\coopnet.cpp src\udp_session.cpp src\coop_clock.cpp src\coop_vote.cpp src\map_labels.cpp
set LIBS=user32.lib
set STEAM_SDK=steamworks_sdk\sdk
if exist %STEAM_SDK%\public\steam\steam_api.h (
    set SOURCES=%SOURCES% src\steam_session.cpp
    set STEAM_FLAGS=/DCOOPNET_STEAM /I%STEAM_SDK%\public /wd4828
    set LIBS=%LIBS% %STEAM_SDK%\redistributable_bin\win64\steam_api64.lib delayimp.lib /DELAYLOAD:steam_api64.dll
    echo Steam: included
) else (
    echo Steam: SDK not found, UDP only
)

cl /nologo /LD /O2 /MD /EHsc /std:c++17 /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS %STEAM_FLAGS% %SOURCES% /Fobuild\ /Fe:build\CoopNet.dll /link /NOLOGO %LIBS% || exit /b 1
copy /y build\CoopNet.dll ..\modules\CoopNet.dll >nul && echo OK: modules\CoopNet.dll
if exist %STEAM_SDK%\redistributable_bin\win64\steam_api64.dll copy /y %STEAM_SDK%\redistributable_bin\win64\steam_api64.dll ..\steam_api64.dll >nul && echo OK: steam_api64.dll
