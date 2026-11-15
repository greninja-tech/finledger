$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location (Join-Path $root 'backend')
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
& .\build\Release\finledger_server.exe
