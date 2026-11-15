$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$backend = Join-Path $root 'backend'
Push-Location $backend
try {
    ctest --test-dir build-mingw --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'C++ Systems Lab tests failed.' }
    $server = Join-Path $backend 'build-mingw\finledger_server.exe'
    if (Test-Path $server) {
        python (Join-Path $backend 'tests\smoke_test.py') $server
        if ($LASTEXITCODE -ne 0) { throw 'HTTP API smoke tests failed.' }
    } else {
        Write-Warning 'HTTP server executable not found; skipped API smoke test.'
    }
} finally { Pop-Location }
