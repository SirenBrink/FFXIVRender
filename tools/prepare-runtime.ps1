param(
    [string]$SdkArchive,
    [string]$StockNrArchive,
    [string]$CompatNrArchive,
    [string]$Compat3108NrArchive,
    [string]$SevenZip = 'C:\Program Files\7-Zip\7z.exe'
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskCache=Join-Path $taskRoot '.cache'
$taskRuntime=Join-Path $taskRoot 'runtime'
New-Item -ItemType Directory -Path $taskCache,$taskRuntime,(Join-Path $taskCache 'ngx-sdk') -Force | Out-Null
if(-not $SdkArchive){
    $SdkArchive=Join-Path $taskCache 'streamline-sdk-v2.14.16.zip'
    if(-not(Test-Path -LiteralPath $SdkArchive)){
        Invoke-WebRequest -Uri 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip' -OutFile $SdkArchive
    }
}
$taskExpectedSdkHash='32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B'
if((Get-FileHash -LiteralPath $SdkArchive -Algorithm SHA256).Hash -ne $taskExpectedSdkHash){
    throw 'SDK archive differs from the pinned official 2.14.16 release. Nothing was extracted.'
}
$taskZip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($SdkArchive))
try {
    foreach($taskEntry in $taskZip.Entries){
        $taskOut=$null
        if($taskEntry.FullName -match '^bin/x64/([^/\\]+\.(dll|json|txt|md))$'){$taskOut=Join-Path $taskRuntime $Matches[1]}
        elseif($taskEntry.FullName -match '^external/ngx-sdk/(include/)?(nvsdk_ngx[^/\\]*\.h|license\.txt)$'){$taskOut=Join-Path (Join-Path $taskCache 'ngx-sdk') $Matches[2]}
        elseif($taskEntry.FullName -eq 'license.txt'){$taskOut=Join-Path $taskRuntime 'streamline.license.txt'}
        if($taskOut){[IO.Compression.ZipFileExtensions]::ExtractToFile($taskEntry,$taskOut,$true)}
    }
} finally {$taskZip.Dispose()}
if($StockNrArchive){
    # Unmodified model is a diagnostic reference only. Production selections use
    # the two user-supplied compatibility models below.
    $taskStock=Join-Path $taskCache 'reference-nr/310.8-stock';New-Item -ItemType Directory -Path $taskStock -Force | Out-Null
    $taskZip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($StockNrArchive))
    try {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($taskZip.GetEntry('nvngx_dlssnr.dll'),(Join-Path $taskStock 'nvngx_dlssnr.dll'),$true)
        [IO.Compression.ZipFileExtensions]::ExtractToFile($taskZip.GetEntry('Other/1. Streamline/sl.dlss_nr.dll'),(Join-Path $taskRuntime 'sl.dlss_nr.dll'),$true)
    } finally {$taskZip.Dispose()}
}
if($CompatNrArchive){
    if(-not(Test-Path -LiteralPath $SevenZip)){throw '7-Zip is required to extract the supplied compatibility RAR'}
    $taskCompat=Join-Path $taskRuntime 'nr/310.9.16-compat';New-Item -ItemType Directory -Path $taskCompat -Force | Out-Null
    & $SevenZip e -y "-o$taskCompat" $CompatNrArchive nvngx_dlssnr.dll
    if($LASTEXITCODE -ne 0){throw 'Compatibility NR extraction failed'}
}
if($Compat3108NrArchive){
    if(-not(Test-Path -LiteralPath $SevenZip)){throw '7-Zip is required to extract the supplied compatibility archive'}
    $taskCompat=Join-Path $taskRuntime 'nr/310.8-compat';New-Item -ItemType Directory -Path $taskCompat -Force | Out-Null
    & $SevenZip e -y "-o$taskCompat" $Compat3108NrArchive nvngx_dlssnr.dll
    if($LASTEXITCODE -ne 0){throw 'Compatibility NR 310.8 extraction failed'}
}
$taskRecords=Get-ChildItem -LiteralPath $taskRuntime -Recurse -File | Where-Object Extension -eq '.dll' | ForEach-Object {
    [pscustomobject]@{file=[IO.Path]::GetRelativePath($taskRuntime,$_.FullName);version=$_.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;bytes=$_.Length}
}
[pscustomobject]@{sdk='2.14.16';sdk_archive_sha256=(Get-FileHash -LiteralPath $SdkArchive -Algorithm SHA256).Hash;modules=@($taskRecords)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskRuntime 'manifest.json') -Encoding utf8
'Prepared private runtime files. No libraries were installed into the game.'
