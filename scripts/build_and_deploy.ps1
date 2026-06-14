param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$PyScript = Join-Path $ScriptDir "build_and_deploy.py"

Write-Host "===========================================" -ForegroundColor Cyan
Write-Host "  OBS Mouse Trail Build & Deploy Launcher " -ForegroundColor Cyan
Write-Host "===========================================" -ForegroundColor Cyan

if (Get-Command py -ErrorAction SilentlyContinue) {
    Write-Host "[INFO] Using Python launcher: py -3" -ForegroundColor Yellow
    & py -3 $PyScript @Args
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[SUCCESS] Build and deploy finished." -ForegroundColor Green
    } else {
        Write-Host "[ERROR] Build and deploy failed with exit code $LASTEXITCODE." -ForegroundColor Red
    }
    exit $LASTEXITCODE
}

if (Get-Command python -ErrorAction SilentlyContinue) {
    Write-Host "[INFO] Using Python executable: python" -ForegroundColor Yellow
    & python $PyScript @Args
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[SUCCESS] Build and deploy finished." -ForegroundColor Green
    } else {
        Write-Host "[ERROR] Build and deploy failed with exit code $LASTEXITCODE." -ForegroundColor Red
    }
    exit $LASTEXITCODE
}

Write-Error "Python was not found. Install Python 3 and retry."
exit 1
