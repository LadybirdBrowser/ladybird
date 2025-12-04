@echo off
REM Ladybird Windows Build Script Wrapper
REM This is a simple wrapper that calls the PowerShell build script

setlocal enabledelayedexpansion

REM Check if PowerShell is available
where powershell >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo Error: PowerShell is not available on this system.
    echo Please install PowerShell or run build-windows.ps1 directly.
    exit /b 1
)

REM Check if build-windows.ps1 exists
if not exist "%~dp0build-windows.ps1" (
    echo Error: build-windows.ps1 not found in the current directory.
    exit /b 1
)

REM Display banner
echo ========================================
echo Ladybird Windows Build
echo ========================================
echo.

REM If no arguments, show help
if "%~1"=="" (
    echo Usage: build-windows.bat [BuildType] [Options]
    echo.
    echo Build Types:
    echo   Release        - Optimized release build (default)
    echo   Debug          - Debug build with symbols
    echo   Distribution   - Distribution build for installer
    echo.
    echo Common Commands:
    echo   build-windows.bat                    - Build release version
    echo   build-windows.bat Debug              - Build debug version
    echo   build-windows.bat Distribution       - Build for distribution
    echo.
    echo For more options, run:
    echo   powershell -ExecutionPolicy Bypass -File build-windows.ps1 -Help
    echo.
    pause
    exit /b 0
)

REM Pass all arguments to PowerShell script
echo Running PowerShell build script...
echo.
powershell -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" %*

REM Check exit code
if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed with error code %ERRORLEVEL%
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo Build completed successfully!
pause
