param(
    [string]$GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIV Online\game',
    [switch]$EnableHdrTest,
    [switch]$EnableFgTest,
    [switch]$EnableFsrMfgTest,
    [switch]$PreflightOnly,
    # Explicit development install when the user chooses gameplay validation
    # instead of desktop FG probes. Never manufacture passing test receipts.
    [switch]$InGameFgTest
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskSm86 = Join-Path $taskRoot 'runtime/fg/dlssg_sm86/dlssg_sm86.dll'
if (-not (Test-Path -LiteralPath $taskSm86) -or
    (Get-FileHash -LiteralPath $taskSm86 -Algorithm SHA256).Hash -ne '7489A89BF593CDA243D95C62E6D8B75905D9774E27B7B2D53B91625A79C05CA4') {
    throw 'Prepare the pinned RTX 20/30 runtime with tools/prepare-sm86-runtime.ps1 before installing this build.'
}
$taskGame = [IO.Path]::GetFullPath($GameDirectory).TrimEnd('\','/')
if (-not (Test-Path -LiteralPath (Join-Path $taskGame 'ffxiv_dx11.exe'))) { throw 'FFXIV executable was not found.' }
if (Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue) { throw 'Close FFXIV before installing the addon.' }
if (-not (Test-Path -LiteralPath (Join-Path $taskGame 'dxgi.dll'))) { throw 'Install the addon-enabled ReShade DXGI loader first.' }
if ((Get-Item -LiteralPath (Join-Path $taskGame 'dxgi.dll')).VersionInfo.ProductName -ne 'ReShade') { throw 'The existing DXGI loader is not identified as ReShade.' }
if (-not (Test-Path -LiteralPath (Join-Path $taskRoot 'build/tests-passed.stamp'))) { throw 'Build and pass the test suite before installing.' }
$taskAddon = Join-Path $taskRoot 'build/XIVRender.addon64'
$taskEnabler = Join-Path $taskRoot 'runtime/fg/dlss-enabler-headless.dll'
if ((Get-FileHash -LiteralPath $taskEnabler).Hash -ne '1A2807D50C13BC4FABC19EED3B50680E2FF49BCF2B35D3505B359E944BC7DFA4') {
    throw 'The private DLSS Enabler adaptation does not match its validated hash; use tools/prepare-enabler-headless.py.'
}
$taskMfg = Get-Content -LiteralPath (Join-Path $taskRoot 'build/enabler-passed.json') -Raw | ConvertFrom-Json
$taskMfgReShade = Get-Content -LiteralPath (Join-Path $taskRoot 'build/enabler-reshade-passed.json') -Raw | ConvertFrom-Json
if (-not $taskMfgReShade.passed) { throw 'Run tools/test-enabler-reshade.ps1 before installation.' }
foreach ($taskInput in $taskMfgReShade.sha256.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath $taskInput.Name).Hash -ne $taskInput.Value) {
        throw 'Enabler ReShade validation inputs changed; repeat the test.'
    }
}
if (-not $taskMfg.passed -or -not $taskMfg.pixel_interpolation_passed -or -not $taskMfg.live_pq_passed -or
    -not $taskMfg.nr_pq_passed -or $taskMfg.sha256.addon -ne (Get-FileHash $taskAddon).Hash -or
    $taskMfg.sha256.runtime -ne (Get-FileHash $taskEnabler).Hash -or
    $taskMfg.sha256.probe -ne (Get-FileHash (Join-Path $taskRoot 'build/fg_probe.exe')).Hash) {
    throw 'Run tools/test-enabler.ps1 for the final build before installing.'
}
if ((Get-Item -LiteralPath $taskAddon).LastWriteTimeUtc -gt (Get-Item -LiteralPath (Join-Path $taskRoot 'build/tests-passed.stamp')).LastWriteTimeUtc) { throw 'The addon changed after the last passing test suite.' }
$taskNrAcceptance = Join-Path $taskRoot 'build/nr-reshade-passed.json'
$taskNrTestHint = 'Run tools/test-nr-with-reshade.ps1 against this build and the game''s current dxgi.dll before installing.'
if (-not (Test-Path -LiteralPath $taskNrAcceptance)) { throw $taskNrTestHint }
try { $taskNrPassed = Get-Content -LiteralPath $taskNrAcceptance -Raw | ConvertFrom-Json } catch { throw $taskNrTestHint }
if ($taskNrPassed.schema -ne 2 -or $taskNrPassed.passed -ne $true -or $taskNrPassed.host_controls_passed -ne $true -or $taskNrPassed.private_device_passed -ne $true) { throw $taskNrTestHint }
$taskNrExpected = [ordered]@{
    addon = $taskAddon
    shim = Join-Path $taskRoot 'build/runtime/XIVRender_nvngx.dll_nr.dll'
    model_default = Join-Path $taskRoot 'runtime/nr/310.9.16-compat/nvngx_dlssnr.dll'
    model_alternative = Join-Path $taskRoot 'runtime/nr/310.8-compat/nvngx_dlssnr.dll'
    probe = Join-Path $taskRoot 'build/nr_pipeline_probe.exe'
    observer = Join-Path $taskRoot 'build/NRDeviceObserver.addon64'
    reshade = Join-Path $taskGame 'dxgi.dll'
}
$taskRest = Join-Path $taskGame 'ReshadeEffectShaderToggler.addon64'
if (Test-Path -LiteralPath $taskRest) {
    $taskNrExpected.rest = $taskRest
    $taskRestConfig = Join-Path $taskGame 'ReshadeEffectShaderToggler.ini'
    if (Test-Path -LiteralPath $taskRestConfig) { $taskNrExpected.rest_config = $taskRestConfig }
    elseif ($taskNrPassed.sha256.rest_config) { throw $taskNrTestHint }
} elseif ($taskNrPassed.sha256.rest) { throw $taskNrTestHint }
foreach ($taskNrEntry in $taskNrExpected.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $taskNrEntry.Value) -or (Get-FileHash -LiteralPath $taskNrEntry.Value -Algorithm SHA256).Hash -ne $taskNrPassed.sha256.($taskNrEntry.Key)) { throw $taskNrTestHint }
}
$taskHdrAcceptance = Join-Path $taskRoot 'build/hdr-reshade-passed.json'
$taskHdrHint = 'Run tools/test-hdr-with-reshade.ps1 against this build and the installed ReShade/REST before installing.'
if (-not (Test-Path -LiteralPath $taskHdrAcceptance)) { throw $taskHdrHint }
$taskHdrPassed = Get-Content -LiteralPath $taskHdrAcceptance -Raw | ConvertFrom-Json
if ($taskHdrPassed.schema -ne 2 -or $taskHdrPassed.deferred_passed -ne $true -or $taskHdrPassed.passed -ne $true -or $taskHdrPassed.effects_initialized -ne $true) { throw $taskHdrHint }
$taskHdrExpected = [ordered]@{
 addon=$taskAddon
 probe=Join-Path $taskRoot 'build/hdr_output_probe.exe'
 observer=Join-Path $taskRoot 'build/NRDeviceObserver.addon64'
 reshade=Join-Path $taskGame 'dxgi.dll'
}
foreach ($taskName in @('rest','rest_config')) {
 if ($taskNrExpected.Contains($taskName)) { $taskHdrExpected[$taskName]=$taskNrExpected[$taskName] }
 elseif ($taskHdrPassed.sha256.$taskName) { throw $taskHdrHint }
}
foreach ($taskEntry in $taskHdrExpected.GetEnumerator()) {
 if ((Get-FileHash -LiteralPath $taskEntry.Value).Hash -ne $taskHdrPassed.sha256.($taskEntry.Key)) { throw $taskHdrHint }
}
# Every production provider/format must have passed the exact final binary,
# installed ReShade and REST combination. This remains separate from gameplay.
if(-not $InGameFgTest){
foreach($taskCase in @('1-sdr','1-pq','2-sdr','2-pq','3-sdr','3-pq','3-scrgb','live-sdr','live-pq','live-scrgb')){
    $taskReceipt=Join-Path $taskRoot ('build/fg-'+$(if($taskCase.StartsWith('live-')){$taskCase}else{'reshade-'+$taskCase})+'-passed.json')
    if(-not(Test-Path -LiteralPath $taskReceipt)){throw "Complete ReShade FG validation: $taskCase"}
    $taskFg=Get-Content -LiteralPath $taskReceipt -Raw | ConvertFrom-Json
    if($taskFg.schema -ne 1 -or $taskFg.passed -ne $true -or $taskFg.resize_passed -ne $true){throw 'FG validation receipt is incomplete.'}
    if($taskCase.StartsWith('live-') -and $taskFg.live -ne $true){throw 'Live provider acceptance did not run.'}
    $taskFgFiles=[ordered]@{addon=$taskAddon;probe=Join-Path $taskRoot 'build/fg_desktop_probe.exe';observer=Join-Path $taskRoot 'build/NRDeviceObserver.addon64';reshade=Join-Path $taskGame 'dxgi.dll'}
    foreach($taskName in @('rest','rest_config')){if($taskNrExpected.Contains($taskName)){$taskFgFiles[$taskName]=$taskNrExpected[$taskName]}elseif($taskFg.sha256.PSObject.Properties[$taskName]){throw 'REST changed after FG validation.'}}
    foreach($taskFile in @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll','fg/libxess_fg.dll','fg/libxell.dll','fg/amd_fidelityfx_framegeneration_dx12.dll')){$taskFgFiles['runtime/'+$taskFile]=Join-Path $taskRoot ('runtime/'+$taskFile)}
    foreach($taskEntry in $taskFgFiles.GetEnumerator()){
        $taskHash=$taskFg.sha256.PSObject.Properties[$taskEntry.Key]
        if(-not $taskHash -or (Get-FileHash -LiteralPath $taskEntry.Value).Hash -ne $taskHash.Value){throw "FG validation is stale: $taskCase/$($taskEntry.Key)"}
    }
}
}
& (Join-Path $taskRoot 'build/fg_native_audit.exe') (Join-Path $taskGame 'ffxiv_dx11.exe') | Out-Null
if($LASTEXITCODE -ne 0){throw 'The native frame boundary no longer validates against installed FFXIV.'}
$taskOutputReceipt = Get-Content -LiteralPath (Join-Path $taskRoot 'build/output-switch-reshade-passed.json') -Raw | ConvertFrom-Json
if (-not $taskOutputReceipt.passed -or $taskOutputReceipt.sha256.addon -ne (Get-FileHash -LiteralPath $taskAddon).Hash -or
    $taskOutputReceipt.sha256.reshade -ne (Get-FileHash -LiteralPath (Join-Path $taskGame 'dxgi.dll')).Hash) {
    throw 'Final-build output-switch validation is missing or stale.'
}
# Verify the complete production input list before changing the installation.
$taskRuntimeFiles = @(
    'sl.interposer.dll','sl.common.dll','sl.dlss.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll',
    'nvngx_dlss.dll','nvngx_dlssg.dll','streamline.license.txt','nvngx_dlss.license.txt','reflex.license.txt',
    'nr/310.9.16-compat/nvngx_dlssnr.dll','nr/310.8-compat/nvngx_dlssnr.dll',
    'fg/libxess_fg.dll','fg/libxell.dll','fg/amd_fidelityfx_framegeneration_dx12.dll',
    'fg/dlss-enabler-headless.dll','fg/enabler-manifest.json',
    'fg/xess.LICENSE.txt','fg/fsr.LICENSE.md','fg/fsr.THIRD_PARTY_NOTICES.md','fg/antilag2.LICENSE.txt','fg/manifest.json',
    'fg/dlssg_sm86/dlssg_sm86.dll','fg/dlssg_sm86/dlssg_sm86.ini','fg/dlssg_sm86/ReShade.ini',
    'fg/dlssg_sm86/README.txt','fg/dlssg_sm86/SHA256SUMS.txt','fg/dlssg_sm86/manifest.json'
)
foreach ($taskRelative in $taskRuntimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskRoot ('runtime/' + $taskRelative)) -PathType Leaf)) {
        throw "Missing production runtime: $taskRelative"
    }
}
foreach ($taskRelative in @('LICENSE','THIRD_PARTY_NOTICES.md','docs/NATIVE_NR.md','docs/NATIVE_HDR.md',
    'docs/FRAME_GENERATION.md','docs/FIRST_TEST.md','docs/FG_DIAGNOSTICS.md','docs/REPAIR_STATUS.md',
    'tools/analyze_fg_recording.py','include/xivrender/renodx/LICENSE','include/xivrender/renodx/NOTICE.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskRoot $taskRelative) -PathType Leaf)) { throw "Missing install input: $taskRelative" }
}
if ($PreflightOnly) {
    Write-Output 'Read-only preflight passed: native signatures, final-build receipts and production inputs verified. No game files changed.'
    return
}
# Detect directory restrictions before touching the previous addon. This is a
# normal write-access check, not an elevation request or permission bypass.
$taskWriteCheck = Join-Path $taskGame ('.xivrender-write-check-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$taskCheckStream = [IO.File]::Open($taskWriteCheck, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
$taskCheckStream.Dispose()
Remove-Item -LiteralPath $taskWriteCheck
$taskBackup = Join-Path $taskRoot ('deployments/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $taskBackup -Force | Out-Null
foreach($taskLog in @('XIVRender.log','ReShade.log','XIVRender.ini','ReShade.ini','ReshadeEffectShaderToggler.ini',
                     'XIVRender/installation.json','XIVRender/runtime/sl.log')){
    $taskSource=Join-Path $taskGame $taskLog
    if(Test-Path -LiteralPath $taskSource){
        $taskSavedLog = Join-Path $taskBackup $taskLog
        New-Item -ItemType Directory -Path (Split-Path $taskSavedLog) -Force | Out-Null
        Copy-Item -LiteralPath $taskSource -Destination $taskSavedLog
    }
}
$taskFiles = [Collections.Generic.List[object]]::new()
$taskMoved = [Collections.Generic.List[object]]::new()

function Destination([string]$Relative) {
    $taskResolved = [IO.Path]::GetFullPath((Join-Path $taskGame $Relative))
    if (-not $taskResolved.StartsWith($taskGame + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Install destination escaped the game directory.' }
    return $taskResolved
}
function InstallFile([string]$Source, [string]$Relative) {
    $taskTarget = Destination $Relative
    $taskSourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $taskTarget) {
        if ((Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash -eq $taskSourceHash) {
            $taskFiles.Add([pscustomobject]@{file=$Relative;sha256=$taskSourceHash;bytes=(Get-Item -LiteralPath $taskTarget).Length})
            return
        }
        $taskSaved = Join-Path $taskBackup ('previous/' + $Relative)
        New-Item -ItemType Directory -Path (Split-Path $taskSaved) -Force | Out-Null
        Copy-Item -LiteralPath $taskTarget -Destination $taskSaved
    }
    New-Item -ItemType Directory -Path (Split-Path $taskTarget) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $taskTarget -Force
    if ((Get-FileHash -LiteralPath $taskTarget -Algorithm SHA256).Hash -ne $taskSourceHash) { throw "Installed file verification failed: $Relative" }
    $taskFiles.Add([pscustomobject]@{file=$Relative;sha256=$taskSourceHash;bytes=(Get-Item -LiteralPath $taskTarget).Length})
}

# Explicit production whitelist. Never copy build directories or mock fixtures.
InstallFile $taskAddon 'XIVRender.addon64'
if (-not (Test-Path -LiteralPath (Destination 'XIVRender.ini'))) {
    $taskConfig = Join-Path $taskBackup 'default-XIVRender.ini'
    & (Join-Path $taskRoot 'build/configure_settings.exe') $taskConfig
    if ($LASTEXITCODE -ne 0) { throw 'Could not prepare default addon settings.' }
    InstallFile $taskConfig 'XIVRender.ini'
}
foreach ($taskRelative in $taskRuntimeFiles) {
    InstallFile (Join-Path (Join-Path $taskRoot 'runtime') $taskRelative) ('XIVRender/runtime/' + $taskRelative)
}
InstallFile (Join-Path $taskRoot 'build/runtime/XIVRender_nvngx.dll_nr.dll') 'XIVRender/runtime/XIVRender_nvngx.dll_nr.dll'
InstallFile (Join-Path $taskRoot 'LICENSE') 'XIVRender/LICENSE'
InstallFile (Join-Path $taskRoot 'THIRD_PARTY_NOTICES.md') 'XIVRender/THIRD_PARTY_NOTICES.md'
InstallFile (Join-Path $taskRoot 'docs/NATIVE_NR.md') 'XIVRender/NATIVE_NR.md'
InstallFile (Join-Path $taskRoot 'docs/NATIVE_HDR.md') 'XIVRender/NATIVE_HDR.md'
InstallFile (Join-Path $taskRoot 'docs/FRAME_GENERATION.md') 'XIVRender/FRAME_GENERATION.md'
InstallFile (Join-Path $taskRoot 'docs/FG_DIAGNOSTICS.md') 'XIVRender/FG_DIAGNOSTICS.md'
InstallFile (Join-Path $taskRoot 'docs/REPAIR_STATUS.md') 'XIVRender/REPAIR_STATUS.md'
InstallFile (Join-Path $taskRoot 'tools/analyze_fg_recording.py') 'XIVRender/tools/analyze_fg_recording.py'
InstallFile (Join-Path $taskRoot 'include/xivrender/renodx/LICENSE') 'XIVRender/renodx/LICENSE'
InstallFile (Join-Path $taskRoot 'include/xivrender/renodx/NOTICE.md') 'XIVRender/renodx/NOTICE.md'
if ($EnableHdrTest) {
    $taskConfig = Destination 'XIVRender.ini'
    $taskSavedConfig = Join-Path $taskBackup 'previous/XIVRender.ini'
    New-Item -ItemType Directory -Path (Split-Path $taskSavedConfig) -Force | Out-Null
    Copy-Item -LiteralPath $taskConfig -Destination $taskSavedConfig -Force
    & (Join-Path $taskRoot 'build/configure_settings.exe') $taskConfig --hdr-test
    if ($LASTEXITCODE -ne 0) { throw 'Could not prepare HDR test settings.' }
    $taskFiles.Add([pscustomobject]@{file='XIVRender.ini';sha256=(Get-FileHash -LiteralPath $taskConfig).Hash;bytes=(Get-Item -LiteralPath $taskConfig).Length})
}
if ($EnableFsrMfgTest) {
    $taskConfig = Destination 'XIVRender.ini'
    $taskSavedConfig = Join-Path $taskBackup 'previous/XIVRender.ini'
    New-Item -ItemType Directory -Path (Split-Path $taskSavedConfig) -Force | Out-Null
    if (-not (Test-Path -LiteralPath $taskSavedConfig)) { Copy-Item -LiteralPath $taskConfig -Destination $taskSavedConfig }
    & (Join-Path $taskRoot 'build/configure_settings.exe') $taskConfig --fsr-mfg
    if ($LASTEXITCODE -ne 0) { throw 'Could not enable the FSR MFG option.' }
    $taskFiles.Add([pscustomobject]@{file='XIVRender.ini';sha256=(Get-FileHash -LiteralPath $taskConfig).Hash;bytes=(Get-Item -LiteralPath $taskConfig).Length})
}
if($EnableFgTest){
    $taskConfig=Destination 'XIVRender.ini'
    $taskSavedConfig=Join-Path $taskBackup 'previous/XIVRender.ini'
    New-Item -ItemType Directory -Path (Split-Path $taskSavedConfig) -Force | Out-Null
    if(-not(Test-Path -LiteralPath $taskSavedConfig)){Copy-Item -LiteralPath $taskConfig -Destination $taskSavedConfig}
    & (Join-Path $taskRoot 'build/configure_settings.exe') $taskConfig --fg-test
    if($LASTEXITCODE -ne 0){throw 'Could not prepare FG test settings.'}
    $taskFiles.Add([pscustomobject]@{file='XIVRender.ini';sha256=(Get-FileHash -LiteralPath $taskConfig).Hash;bytes=(Get-Item -LiteralPath $taskConfig).Length})
}

InstallFile (Join-Path $taskRoot 'docs/FIRST_TEST.md') 'XIVRender/FIRST_TEST.md'
foreach ($taskOldName in @('OptiScalerHudless.addon64')) {
    $taskOld = Destination $taskOldName
    if (Test-Path -LiteralPath $taskOld) {
        $taskSaved = Join-Path $taskBackup ('disabled-addons/' + $taskOldName)
        New-Item -ItemType Directory -Path (Split-Path $taskSaved) -Force | Out-Null
        # Absolute source and target are known within the game/workspace roots.
        Move-Item -LiteralPath $taskOld -Destination $taskSaved
        $taskMoved.Add([pscustomobject]@{original=$taskOld;backup=$taskSaved})
    }
}
Copy-Item -LiteralPath (Join-Path $taskGame 'ReShade.ini') -Destination (Join-Path $taskBackup 'ReShade.ini.reference')
$taskManifest = [pscustomobject]@{
    milestone='0.4.16: FSR 3X/4X through DLSS Enabler with asynchronous presentation; gameplay verification pending';
    fg_validation=$(if($InGameFgTest){'User-selected in-game development test; final desktop FG handoffs are unverified'}else{'Exact-build provider/format/resize/live desktop acceptance passed'});
    game_directory=$taskGame;backup_directory=$taskBackup;
    game_executable_sha256=(Get-FileHash -LiteralPath (Join-Path $taskGame 'ffxiv_dx11.exe') -Algorithm SHA256).Hash;
    reshade_sha256=(Get-FileHash -LiteralPath (Join-Path $taskGame 'dxgi.dll') -Algorithm SHA256).Hash;
    nr_default='310.9.16-compat';nr_alternative='310.8-compat';files=$taskFiles.ToArray();moved=$taskMoved.ToArray()
}
$taskJson = $taskManifest | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $taskBackup 'installation.json'), $taskJson, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Destination 'XIVRender/installation.json'), $taskJson, [Text.UTF8Encoding]::new($false))
Write-Output "Installed verified first-test files. Backup: $taskBackup"
