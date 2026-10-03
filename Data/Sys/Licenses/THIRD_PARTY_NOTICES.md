# Third-party notices and source provenance

PrimedGun Quest is an independent, unofficial Quest standalone derivative of
[PrimedGun by Nobbie](https://github.com/Nobbie248/PrimedGun), based on
[Dolphin ReduX / dolphinXR by iChris4](https://github.com/iChris4/dolphinXR) and
[Dolphin Emulator](https://github.com/dolphin-emu/dolphin). The source baseline for
this port is PrimedGun commit `59ce51ce0f5bf72c3edf31917690c40afa35bd1e`.
Original copyright notices and individual source-file SPDX identifiers are
preserved. The Quest work does not imply endorsement by those projects.

## License and corresponding source

The combined Quest application is distributed under **GNU GPL version 3**; see
[LICENSE](LICENSE). Most Dolphin and PrimedGun code, including the Quest
modifications, is licensed under GPL-2.0-or-later. Individual source files and
third-party components retain their original license terms, as described in
the original [COPYING](COPYING), per-file SPDX identifiers, and dependency
notices. The [LICENSES](LICENSES) directory contains the license texts. The
combined application's GPLv3 distribution does not replace or remove the
original per-file license grants.

Each APK release is accompanied by the matching source tag and source archive
in this repository, including the build scripts. Preserve those materials and
these notices when redistributing a build. Native dependency sources are in
`Externals/`. Android dependencies are identified by version in the Gradle build
files and can be obtained from the source projects listed below.

`Data/Sys/Licenses/` contains complete license and notice texts copied from the
vendored components and published Android artifacts. It is included in the APK
as `assets/Sys/Licenses/`. Some notices cover build tools or optional components
in the source distribution, rather than code linked into every APK.

## Principal native components

| Component | License / source of notices |
| --- | --- |
| Khronos OpenXR loader and Vulkan headers | Apache-2.0 and the notices in `Externals/OpenXR/` and `Externals/Vulkan-Headers/` |
| libadrenotools | BSD-2-Clause; `Externals/libadrenotools/LICENSE` |
| Vulkan Memory Allocator | MIT; `Externals/VulkanMemoryAllocator/LICENSE.txt` |
| fmt, Dear ImGui, ImPlot, pugixml, tinygltf | Respective MIT licenses in `Externals/` |
| glslang | BSD and other notices in `Externals/glslang/` |
| curl and mbed TLS | curl license; Apache-2.0 for mbed TLS; respective `Externals/` notices |
| ENet, xxHash, libspng, LZ4, zlib-ng, minizip-ng, bzip2, zstd | Respective permissive license and copyright texts in `Externals/` |
| LZO, libiconv, FreeSurround | Respective GPL/LGPL notices in `Externals/`; FreeSurround copyright notice is also included in the APK license bundle |
| cubeb, SFML, SDL, RetroAchievements rcheevos, FatFs, watcher, cpp-optparse | Respective license texts and source-file notices in `Externals/` |

Other vendored libraries and test/build components retain the license files and
source notices in their own directories. The complete texts take precedence over
the short descriptions in this table.

## Android components

| Component | License and source |
| --- | --- |
| AndroidX and Android support libraries | Apache-2.0; [AndroidX source](https://android.googlesource.com/platform/frameworks/support/) |
| Material Components for Android 1.13.0 | Apache-2.0; [source](https://github.com/material-components/material-components-android/tree/1.13.0) |
| Kotlin and kotlinx libraries | Apache-2.0; [Kotlin](https://github.com/JetBrains/kotlin), [coroutines](https://github.com/Kotlin/kotlinx.coroutines), [serialization](https://github.com/Kotlin/kotlinx.serialization) |
| Coil 2.7.0 | Apache-2.0; [source](https://github.com/coil-kt/coil/tree/2.7.0) |
| OkHttp and Okio | Apache-2.0; [OkHttp](https://github.com/square/okhttp), [Okio](https://github.com/square/okio) |
| NoNonsense FilePicker 4.2.1 | MPL-2.0; [unmodified corresponding source](https://github.com/spacecowboy/NoNonsense-FilePicker/tree/4.2.1) |
| desugar_jdk_libs 2.1.5 / OpenJDK library subset | GPL-2.0 with Classpath Exception and included assembly exception; [source](https://github.com/google/desugar_jdk_libs) |

Complete Android notices, Kotlin's NOTICE, and the OpenJDK license and exception
are included in `Data/Sys/Licenses/`. Android's system libraries and the Quest
OpenXR runtime are provided by the device, not redistributed by this repository.

## Fonts, firmware replacements, and artwork

- The GameCube fonts are generated from Droid Sans under Apache-2.0. Their
  complete notice is `Data/Sys/GC/font-licenses.txt`.
- The on-screen font is Bitstream Vera Mono. Its terms and copyright are in
  `Data/Sys/Resources/OSD_Font_VeraMono_Copyright.txt`.
- `Data/Sys/GC/dsp_rom.bin` and `dsp_coef.bin` are Dolphin's free replacement DSP
  firmware, with source and provenance in `docs/DSP/free_dsp_rom/`. They are not
  firmware dumped from a user's console.
- Upstream Dolphin interface artwork is retained with Dolphin attribution.
  The weapon-selection text tiles are drawn by the GPL-licensed overlay code.
- Optional cannon texture packs, Samus artwork, extracted weapon icons, and
  gameplay screenshots from the development copy are excluded from this public
  distribution. In-game graphics come from the user's game file.

Metroid, Metroid Prime, Samus, and related names are Nintendo trademarks. Meta
Quest is a Meta trademark. This project is not affiliated with or endorsed by
Nintendo or Meta, and includes no commercial game image, personal save, or
console firmware dump. Users supply their own game file.
