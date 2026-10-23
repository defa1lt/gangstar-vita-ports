# Gangstar Vita ports

A collection of three independent, experimental PS Vita ports for Android versions of games in the Gangstar series:

| Project | Game | Current state |
| --- | --- | --- |
| [`gangstarrio-vita/`](gangstarrio-vita/) | Gangstar Rio: City of Saints | Native startup and world-entry work has been tested on hardware. Long-run stability and driving performance remain open. |
| [`gangstarmiami-vita/`](gangstarmiami-vita/) | Gangstar: Miami Vindication HD | Reaches the menu and playable world on hardware. Further combat, driving, and performance checks remain. |
| [`gangstarvegas-vita/`](gangstarvegas-vita/) | Gangstar Vegas | Native startup, JNI bootstrap, and OBB reads have been observed. A rendered frame and gameplay are not yet confirmed. |

These are unfinished, unofficial community projects. They are not releases by Gameloft, and none should be considered a finished game port.

## Installation

Download the VPK for your game from [Releases](https://github.com/defa1lt/gangstar-vita-ports/releases) and install it through VitaShell. Install and load `kubridge.skprx` as a KERNEL plugin first. Runtime shader compilation requires `libshacccg.suprx` in `ur0:data/` (Miami also accepts `ur0:data/external/`). These system components are not bundled.

Prepare your own game data using the commands below, then copy the **contents** of the prepared output to the game's data directory:

| Game | Vita data directory | Title ID |
| --- | --- | --- |
| Rio | `ux0:data/gangstarrio/` | `GSRV00001` |
| Miami | `ux0:data/gangstarmiami/` | `PSVGMV002` |
| Vegas | `ux0:data/gangstarvegas/` | `GSGV00001` |

The VPK alone does not contain the game. These prereleases were built and package-checked on the host; this publication's freshly rebuilt packages have not been launched on physical Vita hardware. The table above describes earlier hardware observations.

## Build

The projects have separate CMake configurations and build scripts. You need a PS Vita development environment with the softfp VitaSDK used by these Android ARM builds. The SDK is not included. Set `VITASDK` to your local SDK path, then run the commands for the project you want:

```sh
export VITASDK=/path/to/vitasdk-softfp
cd gangstarrio-vita
./scripts/configure.sh Debug
./scripts/build.sh
```

Use `gangstarmiami-vita` or `gangstarvegas-vita` instead of `gangstarrio-vita` to build the other projects. Each directory's `docs/` folder contains project-specific setup and diagnostic notes. Required third-party source code and its license files are included with each project.

## Game data

### Required APK versions

| Game | Supported Android APK | Required native libraries |
| --- | --- | --- |
| Gangstar Rio: City of Saints | **1.2.2b**, versionCode **12221**, ARMv7 | `lib/armeabi-v7a/libgl2jni.so` |
| Gangstar: Miami Vindication HD | **3.1.6**, **Adreno** build | `lib/armeabi/libGangster2.so` |
| Gangstar Vegas | **1.7.0g**, versionCode **17026**, **Google Play** build, ARMv7 | `lib/armeabi-v7a/libGangstar4.so` and `libgenerator.so` |

The version label alone does not establish compatibility. Use the project's verification tools to check the supported binary fingerprints; other versions and MOD APKs are not supported by these profiles.

Rio requires these OBB files:

```text
main.12030.com.gameloft.android.ANMP.GloftG4HM.obb
patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb
```

Vegas requires these OBB files:

```text
main.17026.com.gameloft.android.ANMP.GloftGGHM.obb
patch.17026.com.gameloft.android.ANMP.GloftGGHM.obb
```

Miami requires the matching extracted `Gangstar2/` cache.

### Prepare the data

Run each command from its project directory. Replace `/path/to/` with your local paths; choose a new output directory.

Rio:

```sh
python3 tools/prepare_data.py \
  --apk /path/to/Rio-1.2.2b.apk \
  --main-obb /path/to/main.12030.com.gameloft.android.ANMP.GloftG4HM.obb \
  --patch-obb /path/to/patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb \
  --out dist/data
```

Miami: arrange the APK's library at `/path/to/miami-source/decompiled_miami/lib/armeabi/libGangster2.so` and the cache at `/path/to/miami-source/Gangstar2/`, then run:

```sh
./scripts/prepare_data.sh /path/to/miami-source staging/data
```

Vegas:

```sh
python3 tools/prepare_data.py --mode vita \
  --apk /path/to/Vegas-1.7.0g.apk \
  --main-obb /path/to/main.17026.com.gameloft.android.ANMP.GloftGGHM.obb \
  --patch-obb /path/to/patch.17026.com.gameloft.android.ANMP.GloftGGHM.obb \
  --out dist/data
```

No game APKs, OBB archives, or extracted Android game libraries are included. Prepare data from your own game copy with the verification and preparation tools in the relevant project's `tools/` directory. The tools check the expected game version and binary hashes. Do not add game data, local logs, save files, or generated VPKs to this repository.

## Licensing

Each project retains its own `LICENSE` file, and third-party license files remain alongside their source. Gangstar and Gameloft names and marks belong to their respective owners. Some included LiveArea images use game-related artwork; confirm those assets can be redistributed, or replace them, before making this repository public.
