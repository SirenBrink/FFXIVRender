param([string]$GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIV Online\game')
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild = Join-Path $taskRoot 'build'
if (Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue) { throw 'Close FFXIV before isolated GPU validation.' }
$taskSources = [ordered]@{
    probe = Join-Path $taskBuild 'output_switch_probe.exe'
    addon = Join-Path $taskBuild 'XIVRender.addon64'
    observer = Join-Path $taskBuild 'NRDeviceObserver.addon64'
    reshade = Join-Path $GameDirectory 'dxgi.dll'
}
if ((Get-Item -LiteralPath $taskSources.reshade).VersionInfo.ProductName -ne 'ReShade') {
    throw 'A genuine addon-enabled ReShade is required.'
}
$taskRun = Join-Path $taskBuild ('reshade-output-switch/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $taskRun -Force | Out-Null
foreach ($taskName in @('ReshadeEffectShaderToggler.addon64', 'ReshadeEffectShaderToggler.ini')) {
    $taskSource = Join-Path $GameDirectory $taskName
    if (Test-Path -LiteralPath $taskSource) { $taskSources[$taskName] = $taskSource }
}
$taskHashes = [ordered]@{}
foreach ($taskEntry in $taskSources.GetEnumerator()) {
    $taskHashes[$taskEntry.Key] = (Get-FileHash -LiteralPath $taskEntry.Value -Algorithm SHA256).Hash
    # The probe exercises the production implementation linked from the same
    # library. Loading the complete addon into the synthetic host would apply
    # game signatures to the wrong executable.
    if ($taskEntry.Key -ne 'addon') { Copy-Item -LiteralPath $taskEntry.Value -Destination $taskRun }
}
$taskOutput = Join-Path $taskRun 'output.txt'
& (Join-Path $taskRun 'output_switch_probe.exe') (Join-Path $taskRoot 'runtime') --providers *> $taskOutput
$taskExit = $LASTEXITCODE
Get-Content -LiteralPath $taskOutput
if ($taskExit -ne 0 -or -not (Select-String -LiteralPath $taskOutput -SimpleMatch 'PASS: cold SDR' -Quiet)) {
    throw "Output transition test failed. See $taskOutput"
}
$taskLog = Join-Path $taskRun 'ReShade.log'
if (Select-String -LiteralPath $taskLog -Pattern '\| ERROR \|' -Quiet) {
    throw 'ReShade reported an error during output transition validation.'
}
if (-not (Select-String -LiteralPath $taskLog -SimpleMatch 'NR device observer: D3D11 game device initialized' -Quiet)) {
    throw 'Real ReShade device callbacks were not observed.'
}
if (Select-String -LiteralPath $taskLog -Pattern 'NR device observer: private D3D12 device (exposed|destroyed)' -Quiet) {
    throw 'Private presentation device leaked into ReShade callbacks.'
}
if ($taskSources.Contains('ReshadeEffectShaderToggler.addon64') -and
    -not (Select-String -LiteralPath $taskLog -SimpleMatch 'ReshadeEffectShaderToggler.addon64' -Quiet)) {
    throw 'Installed REST was not observed.'
}
foreach ($taskEntry in $taskSources.GetEnumerator()) {
    if ((Get-FileHash -LiteralPath $taskEntry.Value -Algorithm SHA256).Hash -ne $taskHashes[$taskEntry.Key]) {
        throw 'An input changed during verification.'
    }
}
[ordered]@{
    schema = 1; passed = $true; generated_frames_verified = $false
    tested_utc = [DateTime]::UtcNow.ToString('o'); evidence_directory = $taskRun; sha256 = $taskHashes
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskBuild 'output-switch-reshade-passed.json') -Encoding utf8
Write-Output "Verified twelve live output/provider phases with ReShade. Evidence: $taskRun"
