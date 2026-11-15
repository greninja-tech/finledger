$ErrorActionPreference = 'Stop'
$backend = Join-Path $PSScriptRoot '..\backend'
$exe = Join-Path $backend 'build-mingw\finledger_systems_lab.exe'
if (-not (Test-Path $exe)) { throw "Systems Lab executable not found. Build the backend first: cmake --build build-mingw" }
Push-Location $backend
try {
    & $exe
    Write-Host "`nRun a specific module with: .\build-mingw\finledger_systems_lab.exe lsm | mvcc | shard | raft"
} finally { Pop-Location }
