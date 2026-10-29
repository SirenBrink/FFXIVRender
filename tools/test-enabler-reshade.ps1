param([string]$GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIV Online\game')
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskGame = [IO.Path]::GetFullPath($GameDirectory)
if (Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue) { throw 'Close FFXIV before isolated validation.' }
$taskRun = Join-Path $taskRoot ('build/enabler-reshade/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $taskRun -Force | Out-Null
$taskSources = @((Join-Path $taskRoot 'build/fg_probe.exe'), (Join-Path $taskRoot 'build/NRDeviceObserver.addon64'))
foreach ($taskName in @('dxgi.dll','ReshadeEffectShaderToggler.addon64','ReshadeEffectShaderToggler.ini')) {
    $taskSources += Join-Path $taskGame $taskName
}
$taskHashes = @{}
foreach ($taskSource in $taskSources) {
    $taskHashes[$taskSource] = (Get-FileHash -LiteralPath $taskSource).Hash
    Copy-Item -LiteralPath $taskSource -Destination $taskRun
}
$taskLog = Join-Path $taskRun 'output.txt'
& (Join-Path $taskRun 'fg_probe.exe') (Join-Path $taskRoot 'runtime') 3 pq --wait-focus 1280 720 --through-output live --hidden --enabler --native-xefg *> $taskLog
if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $taskLog -Pattern '^FAIL:' -Quiet) -or
    -not (Select-String -LiteralPath $taskLog -SimpleMatch 'PASS: live provider changes' -Quiet)) { throw "Enabler ReShade test failed: $taskLog" }
$taskReShade = Join-Path $taskRun 'ReShade.log'
foreach ($taskText in @('NR device observer: D3D11 game device initialized', 'ReshadeEffectShaderToggler.addon64', 'Finished exiting.')) {
    if (-not (Select-String -LiteralPath $taskReShade -SimpleMatch $taskText -Quiet)) { throw "Missing ReShade evidence: $taskText" }
}
if (Select-String -LiteralPath $taskReShade -Pattern '\| ERROR \||NR device observer: private D3D12 device (exposed|destroyed)' -Quiet) { throw 'ReShade error or private-device leak.' }
foreach ($taskSource in $taskSources) {
    if ((Get-FileHash -LiteralPath $taskSource).Hash -ne $taskHashes[$taskSource]) { throw 'Test input changed.' }
}
[ordered]@{passed=$true; evidence_directory=$taskRun; sha256=$taskHashes; gameplay='pending'} |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $taskRoot 'build/enabler-reshade-passed.json') -Encoding utf8
Write-Output "Enabler live HDR/ReShade/REST validation passed: $taskRun"
