@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PY_SCRIPT=%SCRIPT_DIR%build_and_deploy.py"

echo ===========================================
echo   OBS Mouse Trail Build ^& Deploy Launcher
echo ===========================================

where py >nul 2>nul
if %ERRORLEVEL% EQU 0 (
  echo [INFO] Using Python launcher: py -3
  py -3 "%PY_SCRIPT%" %*
  if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] Build and deploy finished.
  ) else (
    echo [ERROR] Build and deploy failed with exit code %ERRORLEVEL%.
  )
  exit /b %ERRORLEVEL%
)

where python >nul 2>nul
if %ERRORLEVEL% EQU 0 (
  echo [INFO] Using Python executable: python
  python "%PY_SCRIPT%" %*
  if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] Build and deploy finished.
  ) else (
    echo [ERROR] Build and deploy failed with exit code %ERRORLEVEL%.
  )
  exit /b %ERRORLEVEL%
)

echo [ERROR] Python was not found. Install Python 3 and retry.
exit /b 1
