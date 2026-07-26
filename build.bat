@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "MODE=%~1"
if "%MODE%"=="" set "MODE=dev"

if /I "%MODE%"=="dev" goto :dev
if /I "%MODE%"=="prod" goto :prod
if /I "%MODE%"=="test" goto :test
if /I "%MODE%"=="tests" goto :test
if /I "%MODE%"=="help" goto :usage
if /I "%MODE%"=="--help" goto :usage
if /I "%MODE%"=="-h" goto :usage

echo Unknown build mode: %MODE%
goto :usage_error

:dev
set "BUILD_DIR=_Build\windows-nolauncher"
call :configure
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BUILD_DIR%" --target kyty_emulator --config Release --parallel 8
if errorlevel 1 exit /b %errorlevel%
cmake --install "%BUILD_DIR%" --prefix "%BUILD_DIR%\install"
exit /b %errorlevel%

:prod
set "BUILD_DIR=_Build\windows-prod"
call :configure
if errorlevel 1 exit /b %errorlevel%
cmake --build "%BUILD_DIR%" --target kyty_emulator --config Release --parallel 8
if errorlevel 1 exit /b %errorlevel%
cmake --install "%BUILD_DIR%" --prefix "%BUILD_DIR%\install"
exit /b %errorlevel%

:test
set "BUILD_DIR=_Build\windows-nolauncher"
call :configure
if errorlevel 1 exit /b %errorlevel%

set "TEST_TARGETS=shader_cfg_tests scalar_provenance_tests page_manager_tests memory_tracker_tests shader_vertex_metadata_tests shader_stage_runtime_tests resource_tracking_tests resource_mutex_tests image_page_table_tests shader_recompiler_compute_tests virtual_memory_allocation_tests"
cmake --build "%BUILD_DIR%" --target %TEST_TARGETS% --config Release --parallel 8
if errorlevel 1 exit /b %errorlevel%

for %%T in (%TEST_TARGETS%) do (
	echo.
	echo Running %%T...
	"%BUILD_DIR%\%%T.exe"
	if errorlevel 1 exit /b !errorlevel!
)
exit /b 0

:configure
cmake -S src -B "%BUILD_DIR%" -G Ninja ^
	-DCMAKE_BUILD_TYPE=Release ^
	-DCMAKE_C_COMPILER=clang-cl ^
	-DCMAKE_CXX_COMPILER=clang-cl ^
	-DKYTY_BUILD_LAUNCHER=OFF
exit /b %errorlevel%

:usage
echo Usage: build.bat [dev^|prod^|test]
echo.
echo   dev   Build and install the Release emulator with logging enabled.
echo   prod  Build and install a separate production Release package.
echo   test  Build and run all test executables with logging enabled.
exit /b 0

:usage_error
call :usage
exit /b 2
