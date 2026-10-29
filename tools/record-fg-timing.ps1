param(
    [int]$ProcessId,
    [ValidateRange(15,180)][int]$Seconds = 120,
    [string]$CaptureDirectory,
    [string]$SessionName,
    [switch]$SetupCheck,
    [switch]$Elevate,
    [switch]$Watchdog
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskWpr = Join-Path $env:SystemRoot 'System32/wpr.exe'
$taskNvidia = Join-Path $env:SystemRoot 'System32/nvidia-smi.exe'
$taskPresentMon = Join-Path $taskRoot '.cache/presentmon-2.6.0/PresentMon-2.6.0-x64.exe'
$taskProfile = Join-Path $PSScriptRoot 'XIVRenderTiming.wprp'
$taskGame = 'C:/Program Files (x86)/Steam/steamapps/common/FINAL FANTASY XIV Online/game'
$taskShell = (Get-Process -Id $PID).Path
$taskAdministrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

foreach($taskFile in @($taskWpr,$taskNvidia,$taskPresentMon,$taskProfile)) {
    if(-not (Test-Path -LiteralPath $taskFile)){throw "Missing recorder component: $taskFile"}
}
$taskSignature = Get-AuthenticodeSignature -LiteralPath $taskPresentMon
if($taskSignature.Status -ne 'Valid' -or $taskSignature.SignerCertificate.Subject -notmatch 'O=Intel Corporation') {
    throw 'PresentMon must have a valid Intel signature.'
}
if($SetupCheck) {
    & $taskWpr -profiles $taskProfile
    if($LASTEXITCODE -ne 0){throw 'Windows rejected the recorder profile.'}
    [pscustomobject]@{Ready=$true;AdministratorRequired=$true;AlreadyAdministrator=$taskAdministrator;DurationSeconds=$Seconds;VideoCapture=$false;InputTracking=$false}
    exit 0
}
if(-not $CaptureDirectory){$CaptureDirectory=Join-Path $taskRoot ('diagnostics/live-fg-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$CaptureDirectory=[IO.Path]::GetFullPath($CaptureDirectory)
$taskAllowed=[IO.Path]::GetFullPath((Join-Path $taskRoot 'diagnostics'))+[IO.Path]::DirectorySeparatorChar
if(-not $CaptureDirectory.StartsWith($taskAllowed,[StringComparison]::OrdinalIgnoreCase)) {throw 'Capture output must stay inside project diagnostics.'}
if(-not $SessionName){$SessionName='XIVRender_'+[Guid]::NewGuid().ToString('N')}
if($SessionName -notmatch '^XIVRender_[0-9a-f]{32}$'){throw 'Unexpected recorder session name.'}
New-Item -ItemType Directory -Path $CaptureDirectory -Force | Out-Null
$taskComplete=Join-Path $CaptureDirectory 'complete.txt'
$taskStopFile=Join-Path $CaptureDirectory 'stop.request'

if($Watchdog) {
    # This helper owns only this unique WPR instance, never another recording.
    Start-Sleep -Seconds ($Seconds+15)
    if(-not(Test-Path -LiteralPath $taskComplete)) {
        & $taskWpr -stop (Join-Path $CaptureDirectory 'graphics-watchdog.etl') 'XIVRender watchdog stop' -skipPdbGen -instancename $SessionName *> (Join-Path $CaptureDirectory 'watchdog.log')
    }
    exit 0
}
if($ProcessId -le 0){throw 'Provide the running FFXIV process ID.'}
$taskProcess=Get-Process -Id $ProcessId -ErrorAction Stop
if($taskProcess.ProcessName -ne 'ffxiv_dx11' -or [IO.Path]::GetFullPath($taskProcess.Path) -ne [IO.Path]::GetFullPath((Join-Path $taskGame 'ffxiv_dx11.exe'))) {
    throw 'Recorder target is not the installed FFXIV executable.'
}
if($Elevate -and -not $taskAdministrator) {
    $taskArgs=@('-NoProfile','-File',('"'+$PSCommandPath+'"'),'-ProcessId',"$ProcessId",'-Seconds',"$Seconds",'-CaptureDirectory',('"'+$CaptureDirectory+'"'),'-SessionName',$SessionName)
    $taskHelper=Start-Process -FilePath $taskShell -ArgumentList $taskArgs -Verb RunAs -WindowStyle Hidden -PassThru
    [pscustomobject]@{RecorderProcessId=$taskHelper.Id;CaptureDirectory=$CaptureDirectory;SessionName=$SessionName;DurationSeconds=$Seconds}
    exit 0
}
if(-not $taskAdministrator){throw 'GPU scheduling capture requires Windows administrator approval; use -Elevate.'}
if(Test-Path -LiteralPath (Join-Path $CaptureDirectory 'inputs.json')){throw 'Capture folder already contains a recording; use a new folder.'}

$taskStarted=$false
$taskPm=$null
$taskGpu=$null
$taskClock=[Diagnostics.Stopwatch]::StartNew()
$taskStatusFile=Join-Path $CaptureDirectory 'status.json'
function Write-CaptureStatus([string]$State,[string]$Detail='') {
    $taskInfo=[pscustomobject]@{State=$State;Detail=$Detail;ProcessId=$ProcessId;RecorderProcessId=$PID;SessionName=$SessionName;Utc=[DateTime]::UtcNow.ToString('o');Qpc=[Diagnostics.Stopwatch]::GetTimestamp();QpcFrequency=[Diagnostics.Stopwatch]::Frequency;ElapsedSeconds=$taskClock.Elapsed.TotalSeconds}
    [IO.File]::WriteAllText($taskStatusFile,($taskInfo|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}
try {
    Write-CaptureStatus 'starting'
    $taskAddon=Join-Path $taskGame 'XIVRender.addon64'
    [pscustomobject]@{Version=(Get-Item -LiteralPath $taskAddon).VersionInfo.FileVersion;Sha256=(Get-FileHash -LiteralPath $taskAddon).Hash;PresentMonSha256=(Get-FileHash -LiteralPath $taskPresentMon).Hash;Profile=$taskProfile;ProcessId=$ProcessId;Seconds=$Seconds} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'inputs.json') -Encoding UTF8
    & $taskWpr -start ($taskProfile+'!XIVRender') -filemode -recordtempto $CaptureDirectory -instancename $SessionName *> (Join-Path $CaptureDirectory 'wpr-start.log')
    if($LASTEXITCODE -ne 0){throw ('WPR could not start: '+(Get-Content -LiteralPath (Join-Path $CaptureDirectory 'wpr-start.log') -Raw))}
    $taskStarted=$true
    $taskWatchArgs=@('-NoProfile','-File',('"'+$PSCommandPath+'"'),'-Watchdog','-Seconds',"$Seconds",'-CaptureDirectory',('"'+$CaptureDirectory+'"'),'-SessionName',$SessionName)
    Start-Process -FilePath $taskShell -ArgumentList $taskWatchArgs -WindowStyle Hidden | Out-Null
    $taskPmArgs=@('--process_id',"$ProcessId",'--session_name',$SessionName,'--output_file',('"'+(Join-Path $CaptureDirectory 'presents.csv')+'"'),'--qpc_time','--write_display_metadata','--track_frame_type','--no_track_input','--no_console_stats','--timed',"$Seconds",'--terminate_after_timed','--terminate_on_proc_exit')
    $taskPm=Start-Process -FilePath $taskPresentMon -ArgumentList $taskPmArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $CaptureDirectory 'presentmon.log') -RedirectStandardError (Join-Path $CaptureDirectory 'presentmon-error.log')
    $taskGpuArgs=@('--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,memory.total,clocks.current.graphics,clocks.current.memory,power.draw,pstate,temperature.gpu,clocks_throttle_reasons.active','--format=csv,nounits','--loop-ms=500')
    $taskGpu=Start-Process -FilePath $taskNvidia -ArgumentList $taskGpuArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $CaptureDirectory 'gpu.csv') -RedirectStandardError (Join-Path $CaptureDirectory 'gpu-error.log')
    $taskClock.Restart()
    Write-CaptureStatus 'recording'
    while($taskClock.Elapsed.TotalSeconds -lt $Seconds -and -not(Test-Path -LiteralPath $taskStopFile)) {
        if(-not(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)){break}
        if($taskPm.HasExited -and $taskPm.ExitCode -ne 0){throw ('PresentMon exited: '+(Get-Content -LiteralPath (Join-Path $CaptureDirectory 'presentmon-error.log') -Raw))}
        Start-Sleep -Milliseconds 500
    }
    Write-CaptureStatus 'saving'
} catch {
    Write-CaptureStatus 'error' $_.Exception.Message
    [IO.File]::WriteAllText((Join-Path $CaptureDirectory 'error.txt'),$_.Exception.ToString())
} finally {
    if($taskStarted) {
        & $taskWpr -stop (Join-Path $CaptureDirectory 'graphics.etl') 'XIVRender regular stutter and persistent provider-switch slowdown' -skipPdbGen -instancename $SessionName *> (Join-Path $CaptureDirectory 'wpr-stop.log')
        if($LASTEXITCODE -eq 0){[IO.File]::WriteAllText($taskComplete,[DateTime]::UtcNow.ToString('o'))}
    }
    if($taskGpu -and -not $taskGpu.HasExited){Stop-Process -Id $taskGpu.Id -ErrorAction SilentlyContinue}
    if($taskPm -and -not $taskPm.HasExited) {
        & $taskPresentMon --session_name $SessionName --terminate_existing_session *> (Join-Path $CaptureDirectory 'presentmon-stop.log')
        if(-not $taskPm.WaitForExit(3000)){Stop-Process -Id $taskPm.Id -ErrorAction SilentlyContinue}
    }
    foreach($taskRelative in @('XIVRender.log','ReShade.log','XIVRender.ini','XIVRender/runtime/sl.log')) {
        $taskSource=Join-Path $taskGame $taskRelative
        if(Test-Path -LiteralPath $taskSource){Copy-Item -LiteralPath $taskSource -Destination (Join-Path $CaptureDirectory ([IO.Path]::GetFileName($taskSource))) -ErrorAction SilentlyContinue}
    }
    if((Test-Path -LiteralPath $taskComplete) -and -not(Test-Path -LiteralPath (Join-Path $CaptureDirectory 'error.txt'))){Write-CaptureStatus 'complete'}
}
