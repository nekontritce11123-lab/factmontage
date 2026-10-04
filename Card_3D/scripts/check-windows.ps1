param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    & $Python -B -X utf8 scripts/generate_ui.py --check
    if ($LASTEXITCODE -ne 0) { throw 'Generated contract check failed' }
    & cmd /c scripts\build-windows-tests.cmd 2>&1 | Tee-Object docs/windows_build_log.txt
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    foreach ($test in @('test_public','test_engine','test_geometry','test_installer')) {
        & $Python -B -X utf8 "tests/$test.py" 2>&1 | Tee-Object "docs/windows_$test.txt"
        if ($LASTEXITCODE -ne 0) { throw "$test failed" }
    }
    & ./build/native/memory_smoke.exe 2>&1 | Tee-Object docs/windows_memory_log.txt
    if ($LASTEXITCODE -ne 0) { throw 'Memory guard test failed' }
    if (Test-Path build/abi/frei0r.h) {
        & ./build/native/abi_host.exe build/native/card3d.dll 2>&1 | Tee-Object docs/windows_abi_log.txt
        if ($LASTEXITCODE -ne 0) { throw 'Official ABI host failed' }
    } else {
        'NOT TESTED: official frei0r.h missing in build/abi' | Set-Content docs/windows_abi_log.txt -Encoding utf8
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $toolset = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
    $compiler = Get-Item (Join-Path $toolset.FullName 'bin/Hostx64/x64/cl.exe')
    $record = [ordered]@{
        scope = 'Windows native renderer; not Linux, MLT, Flatpak or Steam Deck'
        date_utc = (Get-Date).ToUniversalTime().ToString('o')
        windows = [System.Environment]::OSVersion.VersionString
        python = (& $Python --version)
        compiler = $compiler.VersionInfo.FileVersion
        binary_sha256 = (Get-FileHash build/native/card3d.dll -Algorithm SHA256).Hash.ToLower()
        official_header_sha256 = $(if (Test-Path build/abi/frei0r.h) { (Get-FileHash build/abi/frei0r.h -Algorithm SHA256).Hash.ToLower() } else { 'unavailable' })
        linux = 'NOT TESTED'
        deck = 'NOT TESTED'
    }
    $record | ConvertTo-Json | Set-Content docs/windows_environment.json -Encoding utf8
} finally { Pop-Location }
