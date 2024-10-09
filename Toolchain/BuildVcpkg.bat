@echo off
setlocal enabledelayedexpansion

REM This script builds the vcpkg dependency management system

REM Get the directory of the script
for %%i in (%~dp0) do set DIR=%%~fi

REM Set the GIT repository and revision
set GIT_REPO=https://github.com/microsoft/vcpkg.git
set GIT_REV=2960d7d80e8d09c84ae8abf15c12196c2ca7d39a
set PREFIX_DIR=%DIR%\Local\vcpkg

REM Create Tarballs directory if it doesn't exist
if not exist "%DIR%\Tarballs" mkdir "%DIR%\Tarballs"

REM Change to Tarballs directory
pushd "%DIR%\Tarballs"

REM Clone vcpkg if not already present
if not exist vcpkg (
    git clone %GIT_REPO%
)

REM Check the current vcpkg version
for /f "delims=" %%i in ('git -C vcpkg rev-parse HEAD') do set bootstrapped_vcpkg_version=%%i

REM If the current version matches the desired revision, exit
if "%bootstrapped_vcpkg_version%" == "%GIT_REV%" (
    exit /b 0
)

REM Build vcpkg
echo Building vcpkg...
cd vcpkg
git fetch origin
git checkout %GIT_REV%

REM Bootstrap vcpkg (disable metrics)
bootstrap-vcpkg.bat -disableMetrics

REM Create the bin directory if it doesn't exist and copy the vcpkg executable
if not exist "%PREFIX_DIR%\bin" mkdir "%PREFIX_DIR%\bin"
copy vcpkg.exe "%PREFIX_DIR%\bin"

popd
