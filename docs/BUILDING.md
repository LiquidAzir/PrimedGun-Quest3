# Build the Quest standalone APK

This repository's supported build target is the **Quest ARM64 APK**. The source includes shared Dolphin and PrimedGun components required by that target. Desktop packaging is outside the scope of this release.

## Dependencies

- Git.
- **JDK 17**, including `java` and `javac`.
- Android SDK **platform 36**.
- Android SDK **Build Tools 35.0.0** and **Platform Tools**.
- Android NDK **29.0.14206865**.
- SDK CMake **3.22.1** and Ninja.
- Network access for the first Gradle/Maven dependency download.

The Gradle wrapper supplies Gradle **9.2.0**; a separate Gradle installation is unnecessary. Required third-party source dependencies are vendored in this repository, so no submodule initialization is needed. Their license notices remain alongside them.

Install the SDK components through Android Studio, or run `sdkmanager` from an installed Android command-line toolchain:

```sh
sdkmanager "platforms;android-36" "build-tools;35.0.0" "platform-tools" "ndk;29.0.14206865" "cmake;3.22.1"
sdkmanager --licenses
```

## Windows

```powershell
git clone https://github.com/LiquidAzir/PrimedGun-Quest3.git
cd PrimedGun-Quest3
.\Source\Android\build-quest.ps1 `
  -JavaHome 'C:\path\jdk-17' `
  -AndroidSdk 'C:\path\android-sdk'
```

Alternatively set `JAVA_HOME` and `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) and run the helper without arguments. The helper checks the JDK/SDK/NDK, writes the ignored local SDK configuration, and builds `:app:assembleQuestPreview`.

Optional Gradle arguments can be passed with `-GradleArguments @('--max-workers=6')`. The first native build is substantial; allow adequate disk space and RAM.

The APK is written to:

```text
Source/Android/app/build/outputs/apk/quest/preview/app-quest-preview.apk
```

## Direct Gradle invocation

With JDK 17 and the SDK configured, run from `Source/Android`:

```sh
./gradlew :app:assembleQuestPreview --no-daemon
```

Use `gradlew.bat` on Windows. The helper-based Windows route is the development path used for this preview; other host platforms have not been validated for this export.

Native code uses optimized **RelWithDebInfo** and includes only **arm64-v8a** libraries. The application package remains `org.primedgun.quest3.debug` for preview update compatibility, but the `preview` build is **not debuggable**.

## Signing and installing

Local preview builds use your local Android debug signing key. Private signing keys and passwords are not in the repository. Keep your key to install updates over your own earlier builds; a different key produces a signature mismatch even when the package name matches. Building a preview from source does not require obtaining the publisher's private key.

Verify a built APK with the SDK's `apksigner`, then follow [installation instructions](INSTALL.md):

```sh
apksigner verify --verbose app-quest-preview.apk
adb install -r -t app-quest-preview.apk
```

Adjust paths to the SDK tool and APK as needed. Do not uninstall a working installation merely to resolve a signature mismatch without first backing up its saves.

The public preview uses an explicit app version and version code rather than deriving update ordering from commit count. Increment the Android version code for future updates. Keep generated build directories, `local.properties`, APKs, signing material, game images, saves, caches, and device logs out of source control.
