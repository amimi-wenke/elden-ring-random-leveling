param(
    [string]$Game,
    [string]$Loader,
    [switch]$Seamless,
    [string]$SeamlessDll,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
if (!$Check -and (Get-Process -Name eldenring -ErrorAction SilentlyContinue)) {
    throw 'Elden Ring is already running. Exit the current game before starting Random Leveling.'
}

$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
if (!$steam) {
    $steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction SilentlyContinue).InstallPath
}
if (!$Game -and $steam) {
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf -PathType Leaf) {
        $libraries += [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '(?m)^\s*"path"\s+"([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value.Replace('\\', '\') }
    }
    $Game = $libraries |
        ForEach-Object { Join-Path $_ 'steamapps\common\ELDEN RING\Game' } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'eldenring.exe') -PathType Leaf } |
        Select-Object -First 1
}
if (!$Game) {
    throw 'Elden Ring was not found in the Steam libraries. Pass -Game "C:\path\to\ELDEN RING\Game".'
}

if (!$Loader) {
    $Loader = (Get-Command me3.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1).Source
    if (!$Loader) {
        $gameLoader = Join-Path $Game 'me3\Windows\me3.exe'
        if (Test-Path -LiteralPath $gameLoader -PathType Leaf) { $Loader = $gameLoader }
    }
}
if (!$Loader -or !(Test-Path -LiteralPath $Loader -PathType Leaf)) {
    throw 'Mod Engine 3 is required. Install it or pass -Loader "C:\path\to\me3.exe".'
}
$Loader = (Resolve-Path -LiteralPath $Loader).Path

$seamlessOptions = @()
if ($Seamless -or $SeamlessDll) {
    if (!$SeamlessDll) { $SeamlessDll = Join-Path $Game 'SeamlessCoop\ersc.dll' }
    if (!(Test-Path -LiteralPath $SeamlessDll -PathType Leaf) -or
        (Split-Path -Leaf $SeamlessDll) -ne 'ersc.dll') {
        throw 'Seamless Co-op is optional. Install it separately or pass -SeamlessDll "C:\path\to\SeamlessCoop\ersc.dll".'
    }
    $SeamlessDll = (Resolve-Path -LiteralPath $SeamlessDll).Path
    $seamlessOptions += @('--native', $SeamlessDll, '--savefile', 'ER0000.onelifecoop.co2')
}

$gameExe = Join-Path $Game 'eldenring.exe'
$profile = Join-Path $PSScriptRoot 'random-leveling.me3'
$mod = Join-Path $PSScriptRoot 'random-leveling-assets\regulation.bin'
$runtime = Join-Path $PSScriptRoot 'runtime\random-leveling.dll'
foreach ($file in @($gameExe, $profile, $mod, $runtime)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing file: $file" }
}
$version = (Get-Item -LiteralPath $gameExe).VersionInfo.FileVersion
if ($version -ne '2.7.2.0') {
    throw "This build is for Elden Ring 1.17.2 Worldwide (2.7.2.0). Found $version."
}
$hashAlgorithm = [Security.Cryptography.SHA256]::Create()
$gameStream = [IO.File]::OpenRead($gameExe)
try {
    $gameHash = [BitConverter]::ToString($hashAlgorithm.ComputeHash($gameStream)).Replace('-', '')
} finally {
    $gameStream.Dispose()
    $hashAlgorithm.Dispose()
}
if ($gameHash -ne 'F9118FEA2CFD8FA3901E8060BFADB1552CB2471771DED8A5B662C4E298FFB328') {
    throw 'This executable differs from the 1.17.2 Worldwide build used by the mod.'
}

$scratch = Join-Path $PSScriptRoot 'state\temp'
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    $env:TEMP = $scratch
    $env:TMP = $scratch
    if ($Check) {
        & $Loader profile show --file $profile
        $result = $LASTEXITCODE
        if ($seamlessOptions.Count) {
            Write-Output "Optional Seamless Co-op DLL: $SeamlessDll"
            Write-Output 'Seamless savefile requested from ME3: ER0000.onelifecoop.co2'
        }
    } else {
        $launchOptions = @()
        if ($steam -and (Test-Path -LiteralPath (Join-Path $steam 'steam.exe') -PathType Leaf)) {
            $launchOptions += @('--steam-dir', $steam)
        }
        & $Loader @launchOptions launch --game eldenring --exe $gameExe --profile $profile --online false @seamlessOptions
        $result = $LASTEXITCODE
    }
} finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
}
exit $result
