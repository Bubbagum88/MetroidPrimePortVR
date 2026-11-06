# PrimedGun 2.0 for Quest 1

Metroid Prime in native VR on the **original Oculus Quest (Quest 1)**.

This is a Quest 1 build of [PrimedGun 2.0](https://github.com/iChris4/MetroidPrimePortVR) by **Nobbie and iChris4**. All the game and VR work is theirs. This fork only changes what the Quest 1 needs to start and draw the game correctly.

**No game content is included.** You need your own Metroid Prime GameCube disc image, USA 1.00 to 1.02 (`GM8E01`).

## What "Quest 1 support" means

The upstream PrimedGun 2.0 APK is built for the Quest 3 and doesn't run on a Quest 1:

- **It closes at launch.** The Quest 1's VR runtime refuses an app whose manifest names a headset it doesn't know (such as `quest3`). This build lists `quest|quest2`.
- **The Quest 1's GPU driver** (Adreno 540, `vulkan.msm8998.so`) crashes or draws wrong on parts of the renderer. This build works around three driver bugs:
  - a crash in `vkCmdDrawIndexed` when a shader asks the length of only some of its storage buffers;
  - `arrayLength()` reading 0 for every storage buffer after the first in a bind group;
  - screen copies (pause background, scan visor, charge beam) coming out as one flat colour.
- **Stereo uses multiview**, because the Quest 1 has no geometry shaders.

The commit messages on this `quest1` branch explain each fix in full.

## Status

Tested on one Quest 1 (firmware v50) with release **v2.0.0-beta.7-quest1**:

- The game runs from the launcher into VR.
- It plays through the opening rooms of the Space Pirate Frigate without crashing.
- The pause menu, scan visor and charge beam draw correctly.

Later areas haven't been played on the Quest 1 yet. That headset is rooted; nothing in the port uses root, but it hasn't been tried on an unrooted Quest 1.

## Requirements

- **Firmware:** tested on Quest 1 firmware **v50** only. Older firmware is untested.
- **Android:** minSdk 29 (Android 10). The Quest 1 on v50 runs Android 10.
- **Graphics:** Vulkan 1.1 (the manifest requires it), using multiview for stereo.
- **OpenXR:** the Khronos OpenXR loader 1.1.61 is bundled in the APK and uses the headset's own OpenXR runtime.

## Install

You need Developer Mode on the headset and either [SideQuest](https://sidequestvr.com/setup-howto) or `adb` (Android platform-tools) on your PC.

1. Download `app-release12.apk` from the [v2.0.0-beta.7-quest1 release](https://github.com/Bubbagum88/MetroidPrimePortVR/releases/tag/v2.0.0-beta.7-quest1).
2. **If another PrimedGun 2.0 APK is installed** (for example upstream's Quest 3 build), it's signed with a different key, so it has to be uninstalled first, and uninstalling deletes its saves. Back them up first (see below).
3. Install the APK with SideQuest's **Install APK** button, or with adb.
4. Copy your disc image to the headset, for example into `/sdcard/Games/`.
5. In the headset, open **PrimedGun** from **App Library > Unknown Sources**. In the launcher panel use **Select Game** to pick your disc image, then press **Play**.

### The same steps with adb

In PowerShell, in your platform-tools folder (where `adb.exe` is), with the APK and your disc image saved there as `app-release12.apk` and `GM8E01.iso`:

```powershell
.\adb install app-release12.apk
.\adb shell mkdir -p /sdcard/Games
.\adb push GM8E01.iso /sdcard/Games/GM8E01.iso
```

If another PrimedGun 2.0 was installed, back up its saves and uninstall it before `.\adb install`:

```powershell
.\adb pull "/sdcard/Android/data/org.primedgun.v2/files/USA/Card A" primedgun-saves
.\adb uninstall org.primedgun.v2
```

After installing and starting the game once, put the saves back:

```powershell
.\adb push primedgun-saves/. "/sdcard/Android/data/org.primedgun.v2/files/USA/Card A"
```

## Known issues

- **Stutter the first time a new shader appears** (it's being compiled). It goes away once that shader is compiled.
- **Don't use the launcher's update check** (Setup > Check for updates). It looks at upstream's releases, which are Quest 3 builds signed with a different key, so they won't install over this one.
- **Signed with a public test key** (in this repository), so CI builds install over each other. Some antivirus and Play Protect checks flag APKs signed this way.

If something breaks, please [open an issue](https://github.com/Bubbagum88/MetroidPrimePortVR/issues) with a log:

```powershell
.\adb logcat -d > primedgun-log.txt
```

## Controls and settings

Controls, the in-headset menu, calibration, saves and texture packs work as in upstream PrimedGun 2.0. See [upstream's README](https://github.com/iChris4/MetroidPrimePortVR#readme). Upstream's notes about the Quest 3, Windows and the update button don't apply to this build.

## Building

The [quest1-apk workflow](.github/workflows/quest1-apk.yml) builds the Quest 1 APK on GitHub Actions on every push to this branch (or by hand from the **Actions** tab). The APK is in the run's `primedgun2-quest1-apk` artifact.

## Credits

- **Nobbie and iChris4**: [PrimedGun 2.0](https://github.com/iChris4/MetroidPrimePortVR), its VR controls and its Quest port. [The original PrimedGun](https://github.com/Nobbie248/PrimedGun) by Nobbie.
- **Odran**: [MetroidPrimePort](https://github.com/Odrannnn/MetroidPrimePort), the native port PrimedGun 2.0 builds on.
- [PrimeDecomp](https://github.com/PrimeDecomp/prime), [Aurora](https://github.com/encounter/aurora), [MusyX](https://github.com/AxioDL/musyx), [nod](https://github.com/encounter/nod) and every other project in upstream's [credits](https://github.com/iChris4/MetroidPrimePortVR#credits).
- Quest 1 build: **Bubbagum88**.

This fork isn't affiliated with or endorsed by PrimeDecomp, Nintendo or Retro Studios. Metroid Prime is a trademark of Nintendo. Licensing is unchanged from upstream: see [LICENSE](LICENSE) and [NOTICE](NOTICE).
