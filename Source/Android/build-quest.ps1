[CmdletBinding()]
param(
    [string]$JavaHome = $env:JAVA_HOME,
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string[]]$GradleArguments = @()
)

$ErrorActionPreference = "Stop"
if (-not $AndroidSdk) { $AndroidSdk = $env:ANDROID_SDK_ROOT }
if (-not $JavaHome) { throw "Set JAVA_HOME or pass -JavaHome with a JDK 17 directory." }
if (-not $AndroidSdk) { throw "Set ANDROID_HOME/ANDROID_SDK_ROOT or pass -AndroidSdk." }

$resolvedJavaHome = (Resolve-Path -LiteralPath $JavaHome).Path
$resolvedAndroidSdk = (Resolve-Path -LiteralPath $AndroidSdk).Path
$javaExecutable = Join-Path $resolvedJavaHome "bin\java.exe"
$javacExecutable = Join-Path $resolvedJavaHome "bin\javac.exe"
foreach ($executable in @($javaExecutable, $javacExecutable)) {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "A complete JDK 17 is required. Missing: $executable"
    }
}

# Java writes version information to stderr. Read it through Process so that Windows
# PowerShell 5.1 does not treat an otherwise successful version check as a native error.
$javaProcess = New-Object System.Diagnostics.Process
$javaProcess.StartInfo.FileName = $javaExecutable
$javaProcess.StartInfo.Arguments = "-version"
$javaProcess.StartInfo.UseShellExecute = $false
$javaProcess.StartInfo.CreateNoWindow = $true
$javaProcess.StartInfo.RedirectStandardOutput = $true
$javaProcess.StartInfo.RedirectStandardError = $true
try {
    [void]$javaProcess.Start()
    $javaVersion = $javaProcess.StandardOutput.ReadToEnd() + $javaProcess.StandardError.ReadToEnd()
    $javaProcess.WaitForExit()
    if ($javaProcess.ExitCode -ne 0 -or $javaVersion -notmatch 'version "17[.\+"]') {
        throw "JDK 17 is required at '$resolvedJavaHome'. Detected: $($javaVersion.Trim())"
    }
}
finally {
    $javaProcess.Dispose()
}

$androidPlatform = Join-Path $resolvedAndroidSdk "platforms\android-36\android.jar"
if (-not (Test-Path -LiteralPath $androidPlatform -PathType Leaf)) {
    throw 'Android SDK platform 36 is missing. Install SDK package "platforms;android-36".'
}
$ndkVersion = "29.0.14206865"
$ndkDirectory = Join-Path $resolvedAndroidSdk "ndk\$ndkVersion"
$ndkProperties = Join-Path $ndkDirectory "source.properties"
if (-not (Test-Path -LiteralPath $ndkProperties -PathType Leaf) -or
    (Get-Content -LiteralPath $ndkProperties -Raw) -notmatch 'Pkg\.Revision\s*=\s*29\.0\.14206865(?:\s|$)') {
    throw "Android NDK $ndkVersion is missing. Install SDK package 'ndk;$ndkVersion'."
}

$gradleWrapper = Join-Path $PSScriptRoot "gradlew.bat"
if (-not (Test-Path -LiteralPath $gradleWrapper -PathType Leaf)) {
    throw "Gradle wrapper not found: $gradleWrapper"
}

# sdk.dir overrides Android SDK environment variables. Keep this ignored, local file
# aligned with the chosen SDK while preserving any other local configuration.
$localPropertiesPath = Join-Path $PSScriptRoot "local.properties"
$localProperties = @()
if (Test-Path -LiteralPath $localPropertiesPath) {
    $localProperties = @(Get-Content -LiteralPath $localPropertiesPath |
        Where-Object { $_ -notmatch '^\s*sdk\.dir\s*=' })
}
$localProperties += "sdk.dir=$($resolvedAndroidSdk.Replace('\', '/'))"
[IO.File]::WriteAllLines($localPropertiesPath, [string[]]$localProperties,
    (New-Object System.Text.UTF8Encoding($false)))

$env:JAVA_HOME = $resolvedJavaHome
$env:ANDROID_HOME = $resolvedAndroidSdk
$env:ANDROID_SDK_ROOT = $resolvedAndroidSdk
$env:ANDROID_NDK_HOME = $ndkDirectory
$env:Path = "$resolvedJavaHome\bin;$resolvedAndroidSdk\platform-tools;$env:Path"

Write-Output "Building PrimedGun Quest with JDK 17 and Android NDK $ndkVersion."
$buildExitCode = 1
Push-Location $PSScriptRoot
try {
    & $gradleWrapper ":app:assembleQuestPreview" "--no-daemon" @GradleArguments
    $buildExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($buildExitCode -ne 0) { exit $buildExitCode }

$apkPath = Join-Path $PSScriptRoot "app\build\outputs\apk\quest\preview\app-quest-preview.apk"
if (-not (Test-Path -LiteralPath $apkPath -PathType Leaf)) {
    throw "Gradle succeeded but the expected APK was not found: $apkPath"
}
Write-Output "APK: $apkPath"
exit 0
