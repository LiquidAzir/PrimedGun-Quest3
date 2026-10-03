# Troubleshooting

## Game not recognized

The supported release is **Metroid Prime GameCube USA, GM8E01, revision 0 (1.0)**. Later revisions, other regions, Wii Trilogy, and Remastered are not supported. The launcher checks the disc metadata; a filename change cannot alter the revision.

For a previously chosen file that moved or lost permission, use **Choose ROM** again. If the system file picker is unavailable, copy the image into the app-created `Games` directory and select **Find copied ROMs**. See [installation](INSTALL.md).

## Launch delay or black screen

Cached shader preparation can take tens of seconds after changes or updates. Keep the headset awake, with tracking available and both controllers connected, and allow startup to finish. Repeatedly pressing Launch will not speed it up.

If the game remains unresponsive, fully quit the app and launch again. Prefer a normal game boot and an in-game memory-card save. Older experimental temporary states can contain an unsafe VR memory layout; the app refuses these rather than resuming corrupted game memory.

If this repeats, record the version, whether it happens before the title screen or after loading a save, and any displayed message. See reporting guidance below.

## Wrong forward direction, height, or movement

Face your intended forward direction, lower the left hand, and click the **right stick**. Quest system recenter is also supported. The visor gesture can suppress right-stick recenter when the left controller is near your head.

For headset-relative movement, open VR settings with the **left menu button** or **left-stick click**, select **Movement → Move Direction → HMD**, then **Save**. Fresh profiles use HMD direction; existing saved preferences are retained. Aim still follows the cannon controller, and lock-on/morph-ball retain their game-specific movement behavior.

## Cannot scan or switch visors

Raise the left controller close to the side of the headset at eye level, then push its stick **left** for Scan Visor. Lower the hand, look toward the target, and hold **left trigger**. Push **up** while the controller is by your head to return to Combat Visor. Thermal and X-ray require their in-game upgrades. See [controls](CONTROLS.md).

## Stutter or low frame rate

The default **2× Full Quality** preserves full shading. The **120 Hz** setting requests the display cadence; it does not make the game simulate or render at 120 FPS. Performance varies by room, effect, loading activity, and headset conditions. Occasional slowdowns and some text/effect issues remain possible; a complete playthrough has not been validated.

Stop emulation before changing **Graphics presets** in the launcher:

| Preset | Tradeoff |
| --- | --- |
| 2× Full Quality | Default; full shading at 2× internal resolution |
| 2× Performance | 2× internal resolution with optional reduced shading detail on eligible scenery |
| 1× Performance | Lower internal resolution and a 72 Hz display request |

All presets use a separate 0.75 eye-buffer scale, multiview rendering, and no MSAA. Higher internal resolution does not eliminate CPU or loading stalls. Keep **GPU texture decoding disabled** and **full-disc RAM caching disabled**; the Quest defaults use compatible CPU texture decoding and disc streaming without changing the original texture content.

## APK update fails

- `unauthorized` in `adb devices`: approve USB debugging inside the headset.
- No device: check developer mode, the data cable, and the operating system's USB/ADB driver.
- Signature mismatch: the installed APK and replacement use different keys. Use an update from the same source, or back up saves before intentionally replacing the installation.
- Version downgrade: use a newer official APK instead of forcing a downgrade over your saved data.

Uninstalling removes app-specific data. [Back up saves first](INSTALL.md#save-quit-and-update).

## Reporting a bug

Open an [issue](https://github.com/LiquidAzir/PrimedGun-Quest3/issues) with:

- APK preview version and headset model.
- Graphics preset and whether you changed movement or other settings.
- Game area and steps to reproduce.
- Whether head tracking continued, whether sound continued, and how long you waited.
- A cropped screenshot if it explains a visual problem.

Do not attach ROMs, game assets, memory cards, temporary states, signing keys, account information, or an entire app-data folder. Logs can contain usernames, file paths, device identifiers, and information from other apps; inspect and redact them before sharing. A short relevant error excerpt is usually enough to begin diagnosis.
