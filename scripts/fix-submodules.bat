@echo off
rem Reset every git submodule to the exact commit recorded in the index, using a
rem shallow fetch. Useful when a submodule directory exists but is empty, or
rem ends up on the wrong commit (e.g. after switching branches).

cd /d "%~dp0.."

for /f "tokens=2,4" %%a in ('git ls-files -s ^| findstr /b "160000"') do (
  echo Checking out %%b @ %%a
  git -C "%%b" fetch --depth 1 origin %%a 2>nul
  if errorlevel 1 (
    echo   fetch failed for %%b
  ) else (
    git -C "%%b" checkout %%a
  )
)