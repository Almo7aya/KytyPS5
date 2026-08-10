@echo off
rem Generic Windows configuration for KytyPS5 (clang-cl + Ninja + Qt 6 + Vulkan).
rem
rem Requirements:
rem   - Visual Studio or Build Tools 2017+ with the C++ toolset installed
rem   - clang-cl, cmake and ninja on PATH (LLVM, CMake, Ninja)
rem   - Qt 6 built for MSVC x64 (auto-detected under C:\Qt, override with QT_DIR)
rem   - Vulkan SDK (VULKAN_SDK set by its installer; optional for configure)
rem
rem Optional overrides (set before running):
rem   QT_DIR     path to the Qt MSVC tree, e.g. C:\Qt\6.8.2\msvc2022_64
rem   BUILD_DIR  output directory, defaults to build

setlocal

if not defined BUILD_DIR set "BUILD_DIR=build"

if not defined QT_DIR (
  for /d %%d in ("C:\Qt\6.*") do set "QT_DIR=%%d\msvc2022_64"
)
if not defined QT_DIR (
  echo ERROR: Qt 6 not found under C:\Qt. Set QT_DIR, e.g. C:\Qt\6.8.2\msvc2022_64.
  exit /b 1
)

for %%t in (cmake ninja clang-cl) do (
  where %%t >nul 2>nul
  if errorlevel 1 (
    echo ERROR: %%t not found on PATH.
    exit /b 1
  )
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found - install Visual Studio or Build Tools with the C++ workload.
  exit /b 1
)
set "VCTOOLSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCTOOLSDIR=%%i"
if not defined VCTOOLSDIR (
  echo ERROR: no Visual Studio installation with the VC C++ tools found.
  exit /b 1
)
call "%VCTOOLSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0.."
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_PREFIX_PATH=%QT_DIR% %*
if errorlevel 1 exit /b 1

echo.
echo Configured into %BUILD_DIR%. Use scripts\build-windows.bat to compile.