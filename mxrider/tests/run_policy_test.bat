@echo off
rem Builds tests\policy_test.exe from the mod's own sources and runs it on a weights file.
rem   run_policy_test.bat path\to\mxrider_policy.txt
rem
rem Finds vcvars64.bat by looking in the usual installs, which avoids depending on vswhere (whose
rem output parsing is fragile when this is launched from a shell that mangles quotes).
setlocal

set "VCVARS="
for %%v in (
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%v set "VCVARS=%%~v"

if not defined VCVARS (
    echo Could not find vcvars64.bat - install the Visual Studio C++ build tools.
    exit /b 1
)
if "%1"=="" (
    echo usage: run_policy_test.bat ^<mxrider_policy.txt^>
    exit /b 2
)

call "%VCVARS%" >nul || exit /b 1
pushd "%~dp0"
if not exist obj mkdir obj
cl /nologo /EHsc /std:c++17 /W4 /MT /I "..\src" /Fe:"policy_test.exe" /Fo:"obj\\" ^
   policy_test.cpp ..\src\policy.cpp ..\src\log.cpp
if errorlevel 1 (popd & exit /b 1)
popd
"%~dp0policy_test.exe" %1
exit /b %errorlevel%
