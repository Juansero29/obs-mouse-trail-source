param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$PyScript = Join-Path $ScriptDir "build_and_deploy.py"

if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 $PyScript @Args
    exit $LASTEXITCODE
}

if (Get-Command python -ErrorAction SilentlyContinue) {
    & python $PyScript @Args
    exit $LASTEXITCODE
}

Write-Error "Python was not found. Install Python 3 and retry."
exit 1
