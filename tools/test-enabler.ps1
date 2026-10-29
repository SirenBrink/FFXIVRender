$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (Get-Process ffxiv_dx11 -ErrorAction SilentlyContinue) { throw 'Close FFXIV before hardware validation.' }
$taskProbe = Join-Path $taskRoot 'build/fg_probe.exe'
$taskRuntime = Join-Path $taskRoot 'runtime'
$taskPython = (Get-Command python -ErrorAction Stop).Source
$taskCases = @(
    @{name='pixels-sdr';args=@('3','sdr','--wait-focus','1280','720','--direct','--hidden','--enabler','--d12-debug')},
    @{name='live-pq';args=@('3','pq','--wait-focus','1280','720','--through-output','live','--hidden','--enabler','--native-xefg')},
    @{name='nr-pq';args=@('3','pq','--wait-focus','1280','720','--direct','live','--hidden','--enabler','--nr-full','--native-xefg')}
)
foreach ($taskCase in $taskCases) {
    $taskLog = Join-Path $taskRoot ('build/enabler-final-' + $taskCase.name + '.log')
    $taskArguments = $taskCase.args
    & $taskProbe $taskRuntime @taskArguments *> $taskLog
    if ($LASTEXITCODE -ne 0 -or (Select-String -LiteralPath $taskLog -Pattern '^FAIL:' -Quiet) -or
        -not (Select-String -LiteralPath $taskLog -Pattern '^PASS:' -Quiet)) { throw "Enabler validation failed: $taskLog" }
    Write-Output "Passed Enabler $($taskCase.name)"
}
$taskPixels = Join-Path $taskRoot 'build/enabler-pixels'
$taskEvidence = Join-Path $taskRoot 'build/enabler-pixels-acceptance.json'
& $taskPython (Join-Path $taskRoot 'tools/validate_enabler_pixels.py') $taskPixels --output $taskEvidence | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Enabler interpolation pixel validation failed.' }
[ordered]@{
    schema=1; passed=$true; pixel_interpolation_passed=$true; live_pq_passed=$true; nr_pq_passed=$true
    tested_utc=[DateTime]::UtcNow.ToString('o')
    sha256=@{
        addon=(Get-FileHash (Join-Path $taskRoot 'build/XIVRender.addon64')).Hash
        probe=(Get-FileHash $taskProbe).Hash
        runtime=(Get-FileHash (Join-Path $taskRuntime 'fg/dlss-enabler-headless.dll')).Hash
    }
    gameplay='pending'
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $taskRoot 'build/enabler-passed.json') -Encoding utf8
