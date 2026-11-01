# Windows entry point. The work is done by gen_cert.py so the result is the
# same on every OS. See gen_cert.py for the outputs.
#
# Usage: .\gen_cert.ps1 [--force-ca]

$ErrorActionPreference = 'Stop'

$python = $null
foreach ($candidate in @('python', 'python3', 'py')) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($cmd) { $python = $cmd.Source; break }
}

if (-not $python) {
    Write-Error 'Python 3 not found. Run this from an ESP-IDF terminal (export.ps1).'
    exit 1
}

& $python (Join-Path $PSScriptRoot 'gen_cert.py') @args
exit $LASTEXITCODE
