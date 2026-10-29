param(
    [switch]$Prepare,
    [switch]$Live,
    [ValidateRange(1,3)][int]$Backend=1,
    [ValidateSet('sdr','pq','scrgb')][string]$Format='pq',
    [ValidateRange(640,7680)][int]$Width=1280,
    [ValidateRange(360,4320)][int]$Height=720,
    [string]$VerifyDirectory,
    [string]$GameDirectory='C:/Program Files (x86)/Steam/steamapps/common/FINAL FANTASY XIV Online/game'
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild=Join-Path $taskRoot 'build'
if(Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue){throw 'Close FFXIV before isolated FG validation.'}
if($Prepare){
    if($Format -eq 'scrgb' -and $Backend -ne 3){throw 'Only FSR FG supports scRGB output.'}
    $taskRun=Join-Path $taskBuild ('reshade-fg-repro/'+$Backend+'-'+$Format+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $taskRun -Force | Out-Null
    $taskSources=[ordered]@{
        addon=Join-Path $taskBuild 'XIVRender.addon64'
        probe=Join-Path $taskBuild 'fg_desktop_probe.exe'
        observer=Join-Path $taskBuild 'NRDeviceObserver.addon64'
        reshade=Join-Path $GameDirectory 'dxgi.dll'
        rest=Join-Path $GameDirectory 'ReshadeEffectShaderToggler.addon64'
        rest_config=Join-Path $GameDirectory 'ReshadeEffectShaderToggler.ini'
    }
    foreach($taskFile in @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll','fg/libxess_fg.dll','fg/libxell.dll','fg/amd_fidelityfx_framegeneration_dx12.dll')){
        $taskSources['runtime/'+$taskFile]=Join-Path $taskRoot ('runtime/'+$taskFile)
    }
    $taskHashes=[ordered]@{}
    foreach($taskEntry in $taskSources.GetEnumerator()){
        if(-not(Test-Path -LiteralPath $taskEntry.Value)){
            if($taskEntry.Key -in @('rest','rest_config')){continue}
            throw "Missing FG test input: $($taskEntry.Value)"
        }
        if($taskEntry.Key -in @('addon','probe','observer') -and (Get-Item $taskEntry.Value).LastWriteTimeUtc -gt (Get-Item (Join-Path $taskBuild 'tests-passed.stamp')).LastWriteTimeUtc){throw 'Rebuild and pass tests before FG validation.'}
        $taskHashes[$taskEntry.Key]=(Get-FileHash -LiteralPath $taskEntry.Value).Hash
        if($taskEntry.Key -in @('probe','observer','reshade','rest','rest_config')){Copy-Item -LiteralPath $taskEntry.Value -Destination $taskRun}
    }
    if((Get-Item $taskSources.reshade).VersionInfo.ProductName -ne 'ReShade'){throw 'A genuine installed ReShade is required.'}
    $taskCase="$Format $Backend $Width $Height 1 " + ((Join-Path $taskRoot 'runtime') -replace '\\','/')
    if($Live){$taskCase+=' live'}
    Set-Content -LiteralPath (Join-Path $taskRun 'fg-desktop-case.txt') -Value $taskCase
    $taskInputs=[pscustomobject]@{backend=$Backend;format=$Format;width=$Width;height=$Height;live=[bool]$Live;sources=$taskSources;sha256=$taskHashes}
    $taskInputs | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskRun 'inputs.json')
    Set-Content -LiteralPath (Join-Path $taskBuild 'fg-current-test-dir.txt') -Value $taskRun
    Write-Output $taskRun
    return
}
if(-not $VerifyDirectory){throw 'Use -Prepare, run its GUI probe, then -VerifyDirectory with the returned folder.'}
$taskRun=[IO.Path]::GetFullPath($VerifyDirectory)
if(-not $taskRun.StartsWith((Join-Path $taskBuild 'reshade-fg-repro')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'FG evidence must remain in the isolated test directory.'}
$taskInputs=Get-Content -LiteralPath (Join-Path $taskRun 'inputs.json') -Raw | ConvertFrom-Json
$taskOutput=Join-Path $taskRun 'fg-desktop-output.txt'
$taskLog=Join-Path $taskRun 'ReShade.log'
$taskDeadline=[DateTime]::UtcNow.AddSeconds(45)
while([DateTime]::UtcNow -lt $taskDeadline){
    if((Test-Path $taskLog) -and (Select-String -LiteralPath $taskLog -SimpleMatch 'Finished exiting.' -Quiet)){break}
    if((Test-Path $taskOutput) -and (Select-String -LiteralPath $taskOutput -SimpleMatch 'FAIL:' -Quiet)){break}
    Start-Sleep -Milliseconds 250
}
foreach($taskText in @('PASS: FG provider presentation','PASS: output resize','SDK output HWND matches=1')){
    if(-not(Select-String -LiteralPath $taskOutput -SimpleMatch $taskText -Quiet)){throw "FG test did not confirm: $taskText"}
}
if($taskInputs.live -and -not(Select-String -LiteralPath $taskOutput -SimpleMatch 'PASS: live provider changes' -Quiet)){throw 'Live provider/latency validation did not pass.'}
if(Select-String -LiteralPath $taskOutput -Pattern 'FAIL:|failures=[1-9]' -Quiet){throw 'FG hardware test failed.'}
if(Select-String -LiteralPath $taskLog -Pattern '\| ERROR \|' -Quiet){throw 'ReShade reported an integration error.'}
foreach($taskText in @('HDR observer: SDR effect runtime initialized','Finished exiting.')){
    if(-not(Select-String -LiteralPath $taskLog -SimpleMatch $taskText -Quiet)){throw "Missing ReShade evidence: $taskText"}
}
if($taskInputs.sha256.rest -and -not(Select-String -LiteralPath $taskLog -SimpleMatch 'Registered add-on "Reshade Effect Shader Toggler"' -Quiet)){throw 'REST did not register.'}
foreach($taskEntry in $taskInputs.sources.PSObject.Properties){
    $taskHash=$taskInputs.sha256.PSObject.Properties[$taskEntry.Name]
    if($taskHash -and (Get-FileHash -LiteralPath $taskEntry.Value).Hash -ne $taskHash.Value){throw 'A source changed during FG validation.'}
}
$taskPassed=[pscustomobject]@{schema=1;passed=$true;resize_passed=$true;live=$taskInputs.live;backend=$taskInputs.backend;format=$taskInputs.format;width=$taskInputs.width;height=$taskInputs.height;tested_utc=[DateTime]::UtcNow.ToString('o');evidence_directory=$taskRun;sha256=$taskInputs.sha256}
$taskReceipt=Join-Path $taskBuild ('fg-reshade-'+$taskInputs.backend+'-'+$taskInputs.format+'-passed.json')
if($taskInputs.live){$taskReceipt=Join-Path $taskBuild ('fg-live-'+$taskInputs.format+'-passed.json')}
[IO.File]::WriteAllText($taskReceipt,($taskPassed|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath $taskOutput -Tail 5
Write-Output "ReShade FG acceptance passed: $taskReceipt"
