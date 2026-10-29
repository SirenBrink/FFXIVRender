param(
    [string]$XeSSArchive,
    [string]$AntiLagArchive,
    [string]$OutputDirectory,
    [switch]$Offline,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskCache = Join-Path $taskRoot '.cache'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $taskRoot 'runtime/fg' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$taskFsrCommit = '60f4ea81909200d8542eca14dccb2628b763a9a3'
$taskSources = [ordered]@{
    xess = [ordered]@{
        version = '3.0.2'
        url = 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip'
        sha256 = '88B8A373F30E33F3558A77A93E634F11B8132FC3047EA1A8EDEEAD32B8471990'
    }
    fsr = [ordered]@{
        version = '2.3.0'
        commit = $taskFsrCommit
        url = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/$taskFsrCommit"
    }
    antilag2 = [ordered]@{
        version = '2.0.0a'
        url = 'https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK/releases/download/v2.0.0a/AntiLag2-SDK.v2.0.0a-390aa4a8.zip'
        sha256 = 'F9C82E1068E7ACEDBFDD05BB2B5F6B63A2BDBBC6AC1ADAAFE399A52B2F559ACA'
    }
}
# These hashes describe unmodified official files. The optional XeFG unlock
# edits only a mapped module after its exact-build/instruction checks pass.
$taskFiles = @(
    [ordered]@{file='libxess_fg.dll';source='xess';entry='bin/libxess_fg.dll';version='1.3.1.78';sha256='EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27';bytes=22957432;license='xess.LICENSE.txt'},
    [ordered]@{file='libxell.dll';source='xess';entry='bin/libxell.dll';version='1.3.2.10';sha256='D2030DCD694FDA8F2EC7E044B13E6DB8F0B56D4BA9113A5EFAD334E3F3DED8C7';bytes=415368;license='xess.LICENSE.txt'},
    [ordered]@{file='xess.LICENSE.txt';source='xess';entry='LICENSE.txt';version=$null;sha256='784BE42C39A4A4D03CF85D88E15E59D75828FCA773CE45B8DBFFFBD7EAF208DF';bytes=4194;license=$null},
    [ordered]@{file='amd_fidelityfx_framegeneration_dx12.dll';source='fsr';entry='Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll';version='4.0.1.2740';sha256='02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18';bytes=40085776;license='fsr.LICENSE.md'},
    [ordered]@{file='fsr.LICENSE.md';source='fsr';entry='Kits/FidelityFX/docs/license.md';version=$null;sha256='6757F2FEC461238DA6F84F83536EC195E8C036B1773A06F6F34301B9ED8C0963';bytes=60891;license=$null},
    [ordered]@{file='fsr.THIRD_PARTY_NOTICES.md';source='fsr';entry='3rdpartynotice.md';version=$null;sha256='94401A90486D1B53B528BB7F845A7B8DAF7227B60EB7C0FC3B8C5571BA202A27';bytes=2441;license=$null},
    [ordered]@{file='antilag2.LICENSE.txt';source='antilag2';entry='LICENSE.txt';version=$null;sha256='DC848DCB4FA7CF9BC50842B4BA44F546D92F39D2957864C34B73AFBA42B5D2BC';bytes=1168;license=$null}
)
function AssertHash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path) -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw "Pinned FG input differs or is missing: $Path"
    }
}
function FetchPinned([string]$Path, [string]$Url, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path)) {
        if ($Offline) { throw "Offline FG input is missing: $Path" }
        New-Item -ItemType Directory -Path (Split-Path $Path) -Force | Out-Null
        $taskPending = $Path + '.pending'
        Invoke-WebRequest -Uri $Url -OutFile $taskPending
        AssertHash $taskPending $Expected
        Move-Item -LiteralPath $taskPending -Destination $Path
    }
    AssertHash $Path $Expected
    return $Path
}
if ($ValidateOnly) {
    foreach ($taskFile in $taskFiles) { AssertHash (Join-Path $OutputDirectory $taskFile.file) $taskFile.sha256 }
    $taskExisting = Get-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Raw | ConvertFrom-Json
    if ($taskExisting.schema -ne 1 -or $taskExisting.modules.Count -ne $taskFiles.Count) { throw 'FG manifest schema or file count differs.' }
    foreach ($taskFile in $taskFiles) {
        $taskMatches = @($taskExisting.modules | Where-Object { $_.file -eq $taskFile.file })
        if ($taskMatches.Count -ne 1 -or $taskMatches[0].sha256 -ne $taskFile.sha256 -or $taskMatches[0].bytes -ne $taskFile.bytes) {
            throw "FG manifest entry differs: $($taskFile.file)"
        }
    }
    'Pinned FG runtime and manifest verified. No files changed.'
    return
}
New-Item -ItemType Directory -Path $taskCache,$OutputDirectory -Force | Out-Null
if (-not $XeSSArchive) { $XeSSArchive = Join-Path $taskCache 'XeSS_SDK_3.0.2.zip' }
if (-not $AntiLagArchive) { $AntiLagArchive = Join-Path $taskCache 'AntiLag2-SDK.v2.0.0a.zip' }
$taskArchives = @{
    xess = FetchPinned ([IO.Path]::GetFullPath($XeSSArchive)) $taskSources.xess.url $taskSources.xess.sha256
    antilag2 = FetchPinned ([IO.Path]::GetFullPath($AntiLagArchive)) $taskSources.antilag2.url $taskSources.antilag2.sha256
}
foreach ($taskFile in $taskFiles) {
    $taskTarget = Join-Path $OutputDirectory $taskFile.file
    if ((Test-Path -LiteralPath $taskTarget) -and (Get-FileHash -LiteralPath $taskTarget).Hash -eq $taskFile.sha256) { continue }
    $taskPending = $taskTarget + '.pending'
    if ($taskFile.source -eq 'fsr') {
        $taskSource = Join-Path (Join-Path $taskCache 'fsr-v2.3.0') $taskFile.entry
        # Earlier dependency preparation saved the two notices with the headers.
        $taskNotice = switch ($taskFile.file) {
            'fsr.LICENSE.md' { Join-Path $taskRoot 'external/fsr/LICENSE.md' }
            'fsr.THIRD_PARTY_NOTICES.md' { Join-Path $taskRoot 'external/fsr/THIRD_PARTY_NOTICES.md' }
        }
        if (-not (Test-Path -LiteralPath $taskSource) -and $taskNotice -and (Test-Path -LiteralPath $taskNotice)) {
            AssertHash $taskNotice $taskFile.sha256
            New-Item -ItemType Directory -Path (Split-Path $taskSource) -Force | Out-Null
            Copy-Item -LiteralPath $taskNotice -Destination $taskSource
        }
        $taskSource = FetchPinned $taskSource ("https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$taskFsrCommit/" + $taskFile.entry) $taskFile.sha256
        Copy-Item -LiteralPath $taskSource -Destination $taskPending -Force
    } else {
        $taskZip = [IO.Compression.ZipFile]::OpenRead($taskArchives[$taskFile.source])
        try {
            $taskEntry = $taskZip.GetEntry($taskFile.entry)
            if (-not $taskEntry) { throw "Pinned archive entry missing: $($taskFile.entry)" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($taskEntry,$taskPending,$true)
        } finally { $taskZip.Dispose() }
    }
    AssertHash $taskPending $taskFile.sha256
    Move-Item -LiteralPath $taskPending -Destination $taskTarget -Force
}
$taskManifest = [ordered]@{
    schema = 1
    sources = $taskSources
    modules = $taskFiles
    streamline = [ordered]@{version='2.14.16';manifest='../manifest.json';prepared_by='tools/prepare-runtime.ps1'}
    xefg_unlock = [ordered]@{optional=$true;on_disk_modified=$false;verified_file_version='1.3.1.78';pe_timestamp='69CB0F4D';image_size='015ED000';source='SirenBrink OptiScaler fork; GPL-3.0-or-later'}
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'manifest.json'),($taskManifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine,[Text.UTF8Encoding]::new($false))
'Prepared pinned FG runtime files and notices. No libraries were installed into the game.'
