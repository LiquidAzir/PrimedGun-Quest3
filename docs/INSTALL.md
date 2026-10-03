# Install on Quest 3

## What you need

- A Quest 3 and both Touch controllers.
- A USB data cable and a computer with [Android Platform Tools](https://developer.android.com/tools/releases/platform-tools), or Meta Quest Developer Hub.
- The APK from this repository's [Releases](https://github.com/LiquidAzir/PrimedGun-Quest3/releases).
- Your own Metroid Prime GameCube USA **GM8E01 revision 0 (1.0)** disc image. ISO, CISO, GCM, RVZ, and GCZ containers are supported. Renaming a different release will not make it compatible.

## Install the APK

Follow [Meta's headset setup instructions](https://developers.meta.com/vr/documentation/native/android/mobile-device-setup/) to enable developer mode. Connect USB and approve USB debugging for your computer in the headset.

From a terminal with `adb` available:

```sh
adb devices
adb install -r -t PrimedGun-Quest3-v0.1.0-preview.1.apk
```

The headset should appear as `device`, not `unauthorized`. Alternatively, install the APK through Meta Quest Developer Hub's device tools. Open **PrimedGun Quest** from the headset's **Unknown Sources** app list.

This preview's package is `org.primedgun.quest3.debug`. The package retains the earlier sideload name for updates, but the distributed preview is not debuggable. Native emulation code is built with optimization enabled.

## Add your game

### Option A: Choose ROM

Copy your disc image onto the headset, open PrimedGun Quest, and select **Choose ROM**. Select the file in the system document picker. Keep it in that location so the saved permission remains useful.

Some Quest installations have no compatible document picker. Use Option B if the app reports this.

### Option B: Copy directly with ADB

Open PrimedGun Quest once and wait for setup to finish; the app must create its own storage folders. Leave emulation stopped. Then copy your game into its `Games` folder, retaining the correct file extension:

```sh
adb push "MetroidPrime.iso" "/sdcard/Android/data/org.primedgun.quest3.debug/files/Games/MetroidPrime.iso"
```

Use `.ciso`, `.gcm`, `.rvz`, or `.gcz` in both filenames instead if that is your actual container. Return to the launcher and select **Find copied ROMs**.

Do not create the app's storage directories with `adb shell mkdir`: folders created under the wrong owner can prevent the app from reading or saving files.

### Windows helper

If you have downloaded the source repository, its helper can install the APK and copy the ROM together:

```powershell
.\tools\deploy-quest.ps1 `
  -Apk '.\PrimedGun-Quest3-v0.1.0-preview.1.apk' `
  -Rom 'C:\path\MetroidPrime.iso'
```

The helper expects `adb` on your PATH; otherwise add `-Adb 'C:\path\platform-tools\adb.exe'`. With multiple devices connected, add `-Serial '<your Quest serial>'`.

## First launch

1. Confirm the launcher recognizes your game, then select **Launch Metroid Prime in VR**.
2. Face the direction you want to use as forward. Lower your left hand and click the right stick to recenter position, direction, and height.
3. Follow the normal game menus. Use **A** to confirm and **B** to go back.
4. Start with the default **2× Full Quality** preset. [Controls](CONTROLS.md) explains scanning, movement, and the VR settings menu.

Shader preparation can delay startup, especially after an update. It does not require repeatedly pressing Launch.

## Save, quit, and update

Use the game's normal save stations or ship save prompt. To return to the launcher, hold the **left menu button and right-stick click together for two seconds**. Save before exiting.

Install later official previews with `adb install -r -t <new-apk>`. Updates signed with the same key preserve app data. Uninstalling the app removes its app-specific data, including saves. A locally built APK may use a different signing key and therefore cannot replace an official preview directly.

Back up the memory-card folder while emulation is stopped:

```sh
adb pull "/sdcard/Android/data/org.primedgun.quest3.debug/files/GC" "PrimedGun-save-backup"
```

The default raw memory card is `GC/MemoryCardA.USA.raw`. To back up preferences as well:

```sh
adb pull "/sdcard/Android/data/org.primedgun.quest3.debug/files/Config" "PrimedGun-config-backup"
```

For an existing compatible Dolphin raw card, the Windows helper accepts `-MemoryCard 'C:\path\MemoryCardA.USA.raw'`. It imports only if the destination card does not already exist, preserving any existing Quest saves.

Use ordinary in-game saves when moving between builds. Temporary states from older experimental builds may be incompatible and are rejected when they contain an unsafe VR memory layout. The Quest app keeps its saves and settings separate from a PC installation.
