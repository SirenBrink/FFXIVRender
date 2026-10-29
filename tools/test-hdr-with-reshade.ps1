param([string]$GameDirectory='C:/Program Files (x86)/Steam/steamapps/common/FINAL FANTASY XIV Online/game')
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild=Join-Path $taskRoot 'build'
if(Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue){throw 'Close FFXIV before running the isolated HDR test.'}
$taskFiles=[ordered]@{
 addon=Join-Path $taskBuild 'XIVRender.addon64'
 probe=Join-Path $taskBuild 'hdr_output_probe.exe'
 observer=Join-Path $taskBuild 'NRDeviceObserver.addon64'
 reshade=Join-Path $GameDirectory 'dxgi.dll'
 rest=Join-Path $GameDirectory 'ReshadeEffectShaderToggler.addon64'
 rest_config=Join-Path $GameDirectory 'ReshadeEffectShaderToggler.ini'
}
if((Get-Item -LiteralPath $taskFiles.reshade).VersionInfo.ProductName -ne 'ReShade'){throw 'A genuine installed ReShade is required.'}
$taskRun=Join-Path $taskBuild ('reshade-hdr-repro/run-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $taskRun -Force | Out-Null
$taskHashes=[ordered]@{}
foreach($taskEntry in $taskFiles.GetEnumerator()){
 if(-not (Test-Path -LiteralPath $taskEntry.Value)){if($taskEntry.Key -in @('rest','rest_config')){continue};throw "Missing test input $($taskEntry.Value)"}
 $taskHashes[$taskEntry.Key]=(Get-FileHash -LiteralPath $taskEntry.Value).Hash
 if($taskEntry.Key -ne 'addon'){Copy-Item -LiteralPath $taskEntry.Value -Destination $taskRun}
}
$taskStamp=Join-Path $taskBuild 'tests-passed.stamp'
foreach($taskName in @('addon','probe','observer')){
 if((Get-Item -LiteralPath $taskFiles[$taskName]).LastWriteTimeUtc -gt (Get-Item -LiteralPath $taskStamp).LastWriteTimeUtc){throw 'Build and pass tests against the final binaries first.'}
}
$taskOutput=Join-Path $taskRun 'output.txt'
& (Join-Path $taskRun 'hdr_output_probe.exe') --hidden --deferred *> $taskOutput
$taskExit=$LASTEXITCODE
Get-Content -LiteralPath $taskOutput
if($taskExit -ne 0 -or -not (Select-String -LiteralPath $taskOutput -SimpleMatch 'PASS: actual HDR swapchain' -Quiet)){throw "HDR hardware probe failed: $taskRun"}
$taskLog=Join-Path $taskRun 'ReShade.log'
if(-not (Test-Path -LiteralPath $taskLog)){throw 'ReShade log was not created.'}
if(Select-String -LiteralPath $taskLog -Pattern '\| ERROR \|' -Quiet){throw "ReShade reported an error: $taskLog"}
if(-not (Select-String -LiteralPath $taskLog -SimpleMatch 'NR device observer: D3D11 game device initialized' -Quiet)){throw 'ReShade device callbacks were not observed.'}
if(-not (Select-String -LiteralPath $taskLog -SimpleMatch 'HDR observer: SDR effect runtime initialized' -Quiet)){throw 'ReShade effects did not initialize.'}
if($taskHashes.Contains('rest') -and -not (Select-String -LiteralPath $taskLog -SimpleMatch 'Registered add-on "Reshade Effect Shader Toggler"' -Quiet)){throw 'REST was not registered.'}
foreach($taskEntry in $taskFiles.GetEnumerator()){
 if($taskHashes.Contains($taskEntry.Key) -and (Get-FileHash -LiteralPath $taskEntry.Value).Hash -ne $taskHashes[$taskEntry.Key]){throw 'A source file changed during the HDR test.'}
}
if(-not (Select-String -LiteralPath $taskOutput -SimpleMatch 'PASS: deferred execution order' -Quiet)){throw 'Deferred HDR/UI acceptance did not run.'}
$taskResult=[pscustomobject]@{schema=2;deferred_passed=$true;passed=$true;effects_initialized=$true;evidence_directory=$taskRun;tested_utc=[DateTime]::UtcNow.ToString('o');sha256=[pscustomobject]$taskHashes}
[IO.File]::WriteAllText((Join-Path $taskBuild 'hdr-reshade-passed.json'),($taskResult|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
Write-Output "HDR ReShade acceptance passed: $taskRun"
