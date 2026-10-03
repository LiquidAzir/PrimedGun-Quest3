param(
    [string]$Apk,
    [string]$Adb = "adb",
    [string]$Serial,
    [string]$Rom,
    [string]$MemoryCard
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$packageName = "org.primedgun.quest3.debug"
if (-not $Apk) {
    $Apk = Join-Path $repoRoot "Source\Android\app\build\outputs\apk\quest\preview\app-quest-preview.apk"
}
foreach ($inputFile in @($Apk, $Rom, $MemoryCard)) {
    if ($inputFile -and -not (Test-Path -LiteralPath $inputFile -PathType Leaf)) {
        throw "File not found: $inputFile"
    }
}
if ([IO.Path]::GetExtension($Apk).ToLowerInvariant() -ne ".apk") {
    throw "The APK file must have an .apk extension."
}
$romExtension = if ($Rom) { [IO.Path]::GetExtension($Rom).ToLowerInvariant() } else { $null }
if ($Rom -and $romExtension -notin @(".ciso", ".iso", ".gcm", ".rvz", ".gcz")) {
    throw "Unsupported disc-image extension: $romExtension"
}
if ($MemoryCard -and [IO.Path]::GetExtension($MemoryCard).ToLowerInvariant() -notin @(".raw", ".gcp")) {
    throw "The memory card must be a Dolphin .raw or .gcp card image."
}

if (-not $Serial) {
    $deviceLines = & $Adb devices -l
    if ($LASTEXITCODE -ne 0) { throw "Could not list ADB devices." }
    $questDevices = @($deviceLines | Where-Object { $_ -match '^\S+\s+device\s+.*model:Quest_3\s' })
    if ($questDevices.Count -ne 1) {
        throw "Specify -Serial for one authorized Quest 3 device. Found $($questDevices.Count)."
    }
    $Serial = ($questDevices[0] -split '\s+')[0]
}

function Invoke-QuestAdb {
    param([string[]]$AdbArguments)
    & $Adb -s $Serial @AdbArguments
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($AdbArguments -join ' ')" }
}

function Test-QuestPath {
    param([string]$Path, [string]$Condition = "-e")
    & $Adb -s $Serial shell test $Condition $Path
    if ($LASTEXITCODE -eq 0) { return $true }
    if ($LASTEXITCODE -eq 1) { return $false }
    throw "Could not inspect Quest path: $Path"
}

function Assert-QuestDirectoryOwner {
    param([string]$Path, [string]$ExpectedOwner)
    if (-not (Test-QuestPath $Path "-d")) {
        throw "The Quest launcher has not created the expected directory: $Path"
    }
    $owner = Invoke-QuestAdb @("shell", "stat", "-c", "%u", $Path)
    if ("$owner".Trim() -ne $ExpectedOwner) {
        throw "Directory $Path has owner $owner; expected app owner $ExpectedOwner. An earlier " +
            "deployment may have created shell-owned directories. Stop the app, back up its " +
            "existing games and saves, and repair the affected ownership before retrying. " +
            "This script will not delete existing data."
    }
}

function Copy-QuestAppFile {
    param([string]$Source, [string]$Target, [switch]$KeepExisting)
    if ($KeepExisting -and (Test-QuestPath $Target)) {
        Write-Output "Keeping the existing Quest memory card: $Target"
        return
    }
    # Stage in the app-created parent directory so an interrupted transfer never
    # replaces a complete ROM or leaves a partial file at the memory-card path.
    $stagingPath = "$Target.import-$([Guid]::NewGuid().ToString('N'))"
    try {
        Invoke-QuestAdb @("push", $Source, $stagingPath)
        # ADB owns uploaded files. ROMs need app read access; freshly imported
        # memory cards also need write access so the game can save. Existing cards
        # are neither overwritten nor chmodded.
        $mode = if ($KeepExisting) { "666" } else { "644" }
        Invoke-QuestAdb @("shell", "chmod", $mode, $stagingPath)
        $moveOption = if ($KeepExisting) { "-n" } else { "-f" }
        Invoke-QuestAdb @("shell", "mv", $moveOption, $stagingPath, $Target)
    } finally {
        # Remove only this invocation's staging file, including after a failed push/copy.
        & $Adb -s $Serial shell rm -f -- $stagingPath
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not remove temporary file: $stagingPath" }
    }
}

Invoke-QuestAdb @("install", "-r", "-t", $Apk)
$userDirectory = "/sdcard/Android/data/$packageName/files"
$launcherComponent = "$packageName/org.dolphinemu.dolphinemu.activities.QuestLauncherActivity"
$packageInfo = Invoke-QuestAdb @("shell", "cmd", "package", "list", "packages", "-U", $packageName)
$packageLine = @($packageInfo | Where-Object { $_ -match "^package:$([regex]::Escape($packageName)) uid:\d+\s*$" })
if ($packageLine.Count -ne 1 -or $packageLine[0] -notmatch ' uid:(\d+)\s*$') {
    throw "Could not identify the installed Quest debug app's UID. Check that -Apk is the Quest debug APK."
}
$appUid = $Matches[1]
$appDirectories = @($userDirectory, "$userDirectory/Config", "$userDirectory/Games", "$userDirectory/GC")
foreach ($directory in $appDirectories) {
    if (Test-QuestPath $directory) { Assert-QuestDirectoryOwner $directory $appUid }
}

# Android must create these directories under its own UID. Shell mkdir and run-as
# are unsuitable here: Quest blocks run-as access to the external storage mount.
Invoke-QuestAdb @("shell", "am", "start", "-W", "-n", $launcherComponent)
$deadline = [DateTime]::UtcNow.AddSeconds(60)
do {
    $missingDirectories = @($appDirectories | Where-Object { -not (Test-QuestPath $_ "-d") })
    if ($missingDirectories.Count -eq 0) { break }
    if ([DateTime]::UtcNow -ge $deadline) {
        throw "The Quest launcher did not finish creating its data directories. Open PrimedGun Quest on the headset, finish any prompts, then retry. Missing: $($missingDirectories -join ', ')"
    }
    Start-Sleep -Seconds 1
} while ($true)
foreach ($directory in $appDirectories) {
    Assert-QuestDirectoryOwner $directory $appUid
}

# Keep the emulator from opening or creating a card while importing files.
Invoke-QuestAdb @("shell", "am", "force-stop", $packageName)
if ($Rom) {
    Copy-QuestAppFile $Rom "$userDirectory/Games/MetroidPrime$romExtension"
}
if ($MemoryCard) {
    $targetCard = "$userDirectory/GC/MemoryCardA.USA.raw"
    Copy-QuestAppFile $MemoryCard $targetCard -KeepExisting
}
Invoke-QuestAdb @("shell", "am", "start", "-W", "-n", $launcherComponent)
Write-Output "Installed PrimedGun Quest on $Serial. Open it from Unknown Sources."
