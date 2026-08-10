@echo off
rem Generic Windows build for KytyPS5. Run scripts\configure-windows.bat first.
rem
rem Optional overrides (must match configure):
rem   BUILD_DIR  output directory, defaults to build

setlocal

if not defined BUILD_DIR set "BUILD_DIR=build"

cd /d "%~dp0.."
cmake --build "%BUILD_DIR%" --target launcher --parallel %*
if errorlevel 1 exit /b 1
cmake --install "%BUILD_DIR%" --prefix "%BUILD_DIR%/install"
if errorlevel 1 exit /b 1

echo.
echo Build complete: %BUILD_DIR%\install