@echo off
rem Builds the MXRider opengl32.dll proxy (x64, Release).
rem
rem The game is x64 (mx.exe machine 0x8664), so only x64 is built: a 32-bit proxy would simply be
rem ignored by the loader. The toolset defaults to v142 (VS2019 Build Tools); pass v143 to override
rem if you only have VS2022:  build.bat v143
setlocal

set "TOOLSET=%~1"
if "%TOOLSET%"=="" set "TOOLSET=v142"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - is Visual Studio or its Build Tools installed?
    exit /b 1
)
set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo MSBuild not found.
    exit /b 1
)

"%MSBUILD%" "%~dp0MXRider.vcxproj" -m -nologo -v:minimal -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=%TOOLSET%
if errorlevel 1 exit /b 1

set "OUT=%~dp0bin\x64\Release"
rem The virtual pad's client library travels with the DLL: the proxy loads it at runtime and keeps
rem working (without driving) if it is absent.
if exist "%~dp0..\reference\vigem\bin\release\x64\ViGEmClient.dll" (
    copy /y "%~dp0..\reference\vigem\bin\release\x64\ViGEmClient.dll" "%OUT%\" >nul
    echo Staged ViGEmClient.dll
)

echo.
echo Built:
echo   %OUT%\opengl32.dll
if exist "%OUT%\ViGEmClient.dll" echo   %OUT%\ViGEmClient.dll
echo.
echo Copy all three (opengl32.dll, ViGEmClient.dll, mxrider.ini) next to mx.exe.
