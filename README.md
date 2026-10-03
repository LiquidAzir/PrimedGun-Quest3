# PrimedGun Quest 3

A standalone Quest 3 preview of PrimedGun, built on Dolphin and Dolphin ReduX, for playing the original GameCube Metroid Prime in VR. Emulation and rendering run on the headset; a PC is only needed for installation and copying your game.

**Preview release: `v0.1.0-preview.1`.** This release is based on the P11 prototype used on Quest 3; the public preview APK has not had an end-to-end headset playthrough. Quest 3S is allowed by the application manifest but has not been validated. This is an experimental community build, not a finished or officially supported port.

## Download and install

1. Download **`PrimedGun-Quest3-v0.1.0-preview.1.apk`** from [Releases](https://github.com/LiquidAzir/PrimedGun-Quest3/releases).
2. Enable Quest developer mode and install the APK with ADB or Meta Quest Developer Hub.
3. Open **PrimedGun Quest** from **Unknown Sources**, then choose your own game file or copy it into the app's `Games` folder.
4. Select **Launch Metroid Prime in VR**. Use both Touch controllers.

**Required game:** Metroid Prime for GameCube, USA **`GM8E01`, revision 0 (1.0)**. Other regions, later revisions, the Wii Trilogy release, and Metroid Prime Remastered are unsupported. The repository and APK do not include a game disc image; provide your own compatible dump.

[Installation and save backups](docs/INSTALL.md) · [Full controls and scanning](docs/CONTROLS.md) · [Troubleshooting](docs/TROUBLESHOOTING.md) · [Build from source](docs/BUILDING.md)

## Essential controls

These are the default right-handed, first-person controls:

| Action | Control |
| --- | --- |
| Move / strafe | Left stick; movement follows the headset direction |
| Turn / jump | Right stick left or right / push right stick up |
| Aim / fire | Right controller / right trigger; hold for a charge shot when available |
| Lock on / scan | Hold left trigger |
| Missiles | Right grip |
| Morph ball / pause | X / Y |
| Map | Left grip |
| Select beam | Hold B, aim toward a beam in the selection panel, release B |
| Recenter | Face forward and click the right stick, with your left hand lowered |
| VR settings | Left menu button or left-stick click |

**To scan:** raise your left controller beside your head, push its stick **left**, then lower it. Look toward the scan target and hold the **left trigger**. Raise the controller again and push **up** to return to the combat visor. [More visor and menu controls](docs/CONTROLS.md).

## Graphics and current limits

The default is **2× Full Quality**, full scene shading, and a **120 Hz display request**. This is the best-looking preset; it does not mean the game produces 120 unique frames per second. The emulated game targets approximately 60 FPS, and demanding scenes or newly encountered effects can still stutter.

The launcher also offers **1× Performance** and **2× Performance** presets. The latter can reduce shading detail on eligible scenery. Keep the default if preserving image quality is your priority. Apply presets while emulation is stopped.

This preview includes the startup, recenter, landing-memory, text-rendering, and performance fixes developed for the Quest version. It has not had a complete game playthrough or comprehensive visor/effect validation. Save regularly at the game's save stations; temporary emulator states are not a substitute for in-game saves.

## Source, credits, and licensing

This repository contains the Quest standalone source and build instructions. Shared emulator code is retained because the Android build requires it; PC builds are outside this repository's release scope.

- [PrimedGun](https://github.com/Nobbie248/PrimedGun), created by Nobbie.
- Dolphin ReduX development by iChris4, and the [Dolphin Emulator project](https://github.com/dolphin-emu/dolphin).
- The Metroid Prime modding community and upstream dependency contributors.

Licensing details are in [COPYING](COPYING), [LICENSES](LICENSES), per-file SPDX notices, and the bundled dependencies' license files. The combined source is GPLv3-compatible; individual files retain their original licenses and attribution. This is an unofficial community project, unaffiliated with Nintendo, Retro Studios, or Meta.

Report bugs through [Issues](https://github.com/LiquidAzir/PrimedGun-Quest3/issues). Include the preview version, headset model, graphics preset, game location, and reproduction steps. Do not upload ROMs, saves, account details, or unreviewed device logs.
