param([Parameter(Mandatory=$true)][string]$Archive)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskOutput = Join-Path $taskRoot 'runtime/fg/dlssg_sm86'
$taskExpected = '7489A89BF593CDA243D95C62E6D8B75905D9774E27B7B2D53B91625A79C05CA4'
$taskZip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Archive))
try {
    $taskEntry = $taskZip.GetEntry('version.dll')
    if (-not $taskEntry -or $taskEntry.Length -ne 33231360) {
        throw 'Expected the supplied SilyNoMeta v0.3.5-5 package. Nothing was installed.'
    }
    $taskSource = $taskEntry.Open()
    $taskHash = [Security.Cryptography.SHA256]::Create()
    try {
        $taskDigest = [BitConverter]::ToString($taskHash.ComputeHash($taskSource)).Replace('-', '')
    } finally { $taskSource.Dispose(); $taskHash.Dispose() }
    if ($taskDigest -ne $taskExpected) { throw 'SM75/SM86 module SHA256 mismatch. Nothing was installed.' }
    New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
    # Keep this private to XIVRender. Never replace ReShade's loader or the
    # game's libraries/configuration, and do not install the second UI addon.
    [IO.Compression.ZipFileExtensions]::ExtractToFile($taskEntry, (Join-Path $taskOutput 'dlssg_sm86.dll'), $true)
    foreach ($taskName in @('dlssg_sm86.ini', 'README.txt', 'SHA256SUMS.txt')) {
        $taskEntry = $taskZip.GetEntry($taskName)
        if (-not $taskEntry) { throw "Package is missing $taskName" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($taskEntry, (Join-Path $taskOutput $taskName), $true)
    }
    # FollowGame is also requested through the live control ABI before ASI
    # initialization. Avoid a second dynamic controller or global hotkeys.
    @'
[DLSSG-SM86-75-COMPANION]
VulkanFamily=0
Multiplier=0
Dynamic=0
TargetFPS=0
DLSSRenderScale=0
UIRecomposition=0
HotkeyX2=
HotkeyX3=
HotkeyX4=
HotkeyX5=
HotkeyX6=
HotkeyDynamic=
HotkeyFollowGame=
HotkeyRestoreINI=
'@ | Set-Content -LiteralPath (Join-Path $taskOutput 'ReShade.ini') -Encoding ascii
    [ordered]@{
        package = 'SilyNoMeta v0.3.5-5'; file = 'dlssg_sm86.dll'; sha256 = $taskExpected
        bytes = 33231360; control = 'FollowGame'; source = 'https://github.com/SilyNoMeta/dlssg_for_sm86'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput 'manifest.json') -Encoding utf8
} finally { $taskZip.Dispose() }
Write-Output "Prepared verified private compatibility runtime at $taskOutput"
