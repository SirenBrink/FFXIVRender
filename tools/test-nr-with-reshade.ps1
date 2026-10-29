param(
    [string]$ReShadeDll = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIV Online\game\dxgi.dll',
    [ValidateRange(128,4096)][int]$Width = 512,
    [ValidateRange(128,4096)][int]$Height = 512
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskReShade = [IO.Path]::GetFullPath($ReShadeDll)
$taskBuild = Join-Path $taskRoot 'build'
$taskTestStamp = Join-Path $taskBuild 'tests-passed.stamp'
$taskAcceptance = Join-Path $taskBuild 'nr-reshade-passed.json'
if (Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue) { throw 'Close FFXIV before running the isolated NR hardware test.' }
if (-not (Test-Path -LiteralPath $taskTestStamp)) { throw 'Build and pass the test suite before running the ReShade NR test.' }
if (-not (Test-Path -LiteralPath $taskReShade) -or (Get-Item -LiteralPath $taskReShade).VersionInfo.ProductName -ne 'ReShade') { throw 'Supply a genuine addon-enabled ReShade dxgi.dll with -ReShadeDll.' }
$taskSources = [ordered]@{
    addon = Join-Path $taskBuild 'XIVRender.addon64'
    shim = Join-Path $taskBuild 'runtime/XIVRender_nvngx.dll_nr.dll'
    model_default = Join-Path $taskRoot 'runtime/nr/310.9.16-compat/nvngx_dlssnr.dll'
    model_alternative = Join-Path $taskRoot 'runtime/nr/310.8-compat/nvngx_dlssnr.dll'
    probe = Join-Path $taskBuild 'nr_pipeline_probe.exe'
    observer = Join-Path $taskBuild 'NRDeviceObserver.addon64'
    reshade = $taskReShade
}
$taskRest = Join-Path (Split-Path $taskReShade) 'ReshadeEffectShaderToggler.addon64'
$taskRestConfig = Join-Path (Split-Path $taskReShade) 'ReshadeEffectShaderToggler.ini'
if (Test-Path -LiteralPath $taskRest) {
    $taskSources.rest = $taskRest
    if (Test-Path -LiteralPath $taskRestConfig) { $taskSources.rest_config = $taskRestConfig }
}
$taskHashes = [ordered]@{}
foreach ($taskEntry in $taskSources.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $taskEntry.Value)) { throw "Required test file is missing: $($taskEntry.Value)" }
    if ($taskEntry.Key -in @('addon','shim','probe','observer') -and (Get-Item -LiteralPath $taskEntry.Value).LastWriteTimeUtc -gt (Get-Item -LiteralPath $taskTestStamp).LastWriteTimeUtc) { throw 'The build changed after its last passing test suite. Rebuild and test first.' }
    $taskHashes[$taskEntry.Key] = (Get-FileHash -LiteralPath $taskEntry.Value -Algorithm SHA256).Hash
}
# A fresh executable directory loads only the observer and an explicit copy of
# installed REST/config, with no game process or preset. Outputs stay here.
$taskRun = Join-Path $taskBuild ('reshade-nr-repro/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$taskProbeRoot = Join-Path $taskBuild 'nr-pipeline-probe-root'
New-Item -ItemType Directory -Path $taskRun -Force | Out-Null
if (Test-Path -LiteralPath $taskAcceptance) {
    Move-Item -LiteralPath $taskAcceptance -Destination (Join-Path $taskRun 'previous-acceptance.json')
}
function StageVerified([string]$Source, [string]$Target, [string]$Hash) {
    New-Item -ItemType Directory -Path (Split-Path $Target) -Force | Out-Null
    if (-not (Test-Path -LiteralPath $Target) -or (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $Hash) {
        Copy-Item -LiteralPath $Source -Destination $Target -Force
    }
    if ((Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $Hash) { throw "Staged test file verification failed: $Target" }
}
$taskProbeExe = Join-Path $taskRun 'nr_pipeline_probe.exe'
StageVerified $taskSources.probe $taskProbeExe $taskHashes.probe
StageVerified $taskSources.reshade (Join-Path $taskRun 'dxgi.dll') $taskHashes.reshade
StageVerified $taskSources.observer (Join-Path $taskRun 'NRDeviceObserver.addon64') $taskHashes.observer
if ($taskSources.Contains('rest')) {
    StageVerified $taskSources.rest (Join-Path $taskRun 'ReshadeEffectShaderToggler.addon64') $taskHashes.rest
    if ($taskSources.Contains('rest_config')) {
        StageVerified $taskSources.rest_config (Join-Path $taskRun 'ReshadeEffectShaderToggler.ini') $taskHashes.rest_config
    }
}
StageVerified $taskSources.shim (Join-Path $taskProbeRoot 'XIVRender/runtime/XIVRender_nvngx.dll_nr.dll') $taskHashes.shim
StageVerified $taskSources.model_default (Join-Path $taskProbeRoot 'XIVRender/runtime/nr/310.9.16-compat/nvngx_dlssnr.dll') $taskHashes.model_default
StageVerified $taskSources.model_alternative (Join-Path $taskProbeRoot 'XIVRender/runtime/nr/310.8-compat/nvngx_dlssnr.dll') $taskHashes.model_alternative
$taskOutput = Join-Path $taskRun 'pipeline.txt'
& $taskProbeExe $taskProbeRoot $Width $Height *> $taskOutput
$taskProbeExit = $LASTEXITCODE
Get-Content -LiteralPath $taskOutput
if ($taskProbeExit -ne 0) { throw "The ReShade NR pipeline test failed ($taskProbeExit). See $taskOutput" }
if (-not (Select-String -LiteralPath $taskOutput -SimpleMatch 'PASS: complete production NR pipeline' -Quiet)) { throw 'The hardware probe did not report a complete pipeline pass.' }
$taskReShadeLog = Join-Path $taskRun 'ReShade.log'
function VerifyPrivateDevice([string]$Log) {
    if (-not (Test-Path -LiteralPath $Log) -or -not (Select-String -LiteralPath $Log -SimpleMatch 'NR device observer: D3D11 game device initialized' -Quiet)) { throw 'The test did not confirm real ReShade device callbacks.' }
    if (Select-String -LiteralPath $Log -Pattern 'NR device observer: private D3D12 device (exposed|destroyed)' -Quiet) { throw 'Private NR device lifetime leaked into ReShade addon callbacks.' }
    if ($taskSources.Contains('rest') -and -not (Select-String -LiteralPath $Log -SimpleMatch 'ReshadeEffectShaderToggler.addon64' -Quiet)) { throw 'Installed REST was not observed in the isolated test.' }
}
VerifyPrivateDevice $taskReShadeLog
Copy-Item -LiteralPath $taskReShadeLog -Destination (Join-Path $taskRun 'pipeline-ReShade.log')
$taskHostOutput = Join-Path $taskRun 'host-controls.txt'
& $taskProbeExe $taskProbeRoot $Width $Height --host-control-test *> $taskHostOutput
$taskHostExit = $LASTEXITCODE
Get-Content -LiteralPath $taskHostOutput
if ($taskHostExit -ne 0 -or -not (Select-String -LiteralPath $taskHostOutput -SimpleMatch 'PASS: production NR host' -Quiet)) { throw "The production NR UI/render-thread test failed. See $taskHostOutput" }
VerifyPrivateDevice $taskReShadeLog
foreach ($taskEntry in $taskSources.GetEnumerator()) {
    if ((Get-FileHash -LiteralPath $taskEntry.Value -Algorithm SHA256).Hash -ne $taskHashes[$taskEntry.Key]) { throw 'A source file changed during the test; repeat it against the final build.' }
}
$taskResult = [pscustomobject]@{
    schema = 2
    passed = $true
    host_controls_passed = $true
    private_device_passed = $true
    width = $Width
    height = $Height
    tested_utc = [DateTime]::UtcNow.ToString('o')
    evidence_directory = $taskRun
    sha256 = [pscustomobject]$taskHashes
}
[IO.File]::WriteAllText($taskAcceptance, ($taskResult | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
Write-Output "ReShade NR acceptance written: $taskAcceptance"
