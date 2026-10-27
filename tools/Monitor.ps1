$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$Host.UI.RawUI.WindowTitle = 'wifiblu - LIVE ESP32 logs (COM6)'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    $toolsDir = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { 'C:\Espressif\tools' } # EIM default
    & (Join-Path $toolsDir 'python\v6.1\venv\Scripts\python.exe') (Join-Path $PSScriptRoot 'live_serial.py') --port COM6
}
finally { Pop-Location }
