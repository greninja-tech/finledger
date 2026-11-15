$ErrorActionPreference = 'Stop'
$backend = Join-Path $PSScriptRoot '..\backend'
Push-Location $backend
try { ctest --test-dir build-mingw --output-on-failure } finally { Pop-Location }
