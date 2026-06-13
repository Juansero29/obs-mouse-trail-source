@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PY_SCRIPT=%SCRIPT_DIR%build_and_deploy.py"

where py >nul 2>nul
if %ERRORLEVEL% EQU 0 (
  py -3 "%PY_SCRIPT%" %*
  exit /b %ERRORLEVEL%
)

where python >nul 2>nul
if %ERRORLEVEL% EQU 0 (
  python "%PY_SCRIPT%" %*
  exit /b %ERRORLEVEL%
)

echo Python was not found. Install Python 3 and retry.
exit /b 1
