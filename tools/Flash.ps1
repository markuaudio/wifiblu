param([string]$Port = 'COM6')
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    . (Join-Path $PSScriptRoot 'Enter-EspIdf.ps1')
    idf.py -p $Port flash
    if ($LASTEXITCODE -ne 0) { throw "Flash failed (exit $LASTEXITCODE). Close the live monitor if it holds $Port." }
}
finally {
    Pop-Location
}
