# Android lifecycle reference

The clean APK's smali and the native disassembly establish the startup order
used by the supported `libgl2jni.so`. The Vita wrapper follows this sequence
and records each boundary in `loader.log`.

## Paths

`GloftG4HM` loads the `SDFolder` preference. Its default is:

```text
/sdcard/Android/data/com.gameloft.android.ANMP.GloftG4HM
```

It passes that value to `GL2JNILib.setResourcePath`. Later,
`GL2JNILib.setupPaths` calls the native `setPaths` entry point with three
strings:

1. resource/data root (`SDFolder`);
2. Android application's files directory;
3. Android application's cache directory.

If no resource root was set, `setupPaths` falls back to:

```text
/sdcard/gameloft/games/GloftG4HM
```

The Vita lifecycle should pass these equivalents:

```text
resource: ux0:data/gangstarrio/game/
files:    ux0:data/gangstarrio/files/
cache:    ux0:data/gangstarrio/cache/
```

The APK also identifies these two proprietary expansion containers:

```text
main.12030.com.gameloft.android.ANMP.GloftG4HM.obb
patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb
```

## Native call order

1. Load and relocate the library. Before running its constructors, redirect
   the hard-coded Android kernel helpers at `0xffff0fa0` and `0xffff0fc0` to
   Vita implementations. Rio contains 4 memory-barrier literals and 43 atomic
   compare/exchange literals. The 00.09 hardware dump reached one from
   `glf::ThreadMgr` while creating the `glf::App`.
2. Run the library constructors, preload the Vita shader compiler, initialize
   FalsoJNI and controls, and call `JNI_OnLoad`. This is the shared loader
   order used by both working reference ports.
3. Call `GameInstaller.initNative()` to retain the installer JNI environment,
   class, and the `startGame` and `getSDFolder` method IDs.
4. Call `GameInstaller.getPublicKey()`, which initializes `lockPointer1`.
5. Supply `lockPointer3`, the one-word start gate normally allocated by
   `GDRMPolicy.nativeAllow()` after the Android licensing callback. The Vita
   wrapper supplies the same value after validating the exact clean game
   library against its pinned size and SHA-256.
6. Call `GameInstaller.nativeStart()`, which consumes `lockPointer3`, creates
   `lockPointer4`, and invokes the Java `startGame()` callback.
   `GL2JNILib.init()` dereferences `lockPointer4`, so skipping this prelude
   causes a data abort at native address `0x37256c`.
7. Call `GL2JNILib.init()` once while the activity creates the game view.
8. Initialize VitaGL, following the renderer bootstrap used by both the 9mm
   and MC2 Vita ports.
9. Call `GL2JNILib.initGL()` while selecting the EGL configuration.
10. Call `GL2JNILib.getViewSettings()`. Its native implementation creates the
   `glf::App`, calls back into Java `setupPaths()`, and then runs
   `App::MyInit()`. The callback must re-enter `GL2JNILib.setPaths(...)` with
   the three Vita paths while the new `App` and filesystem object are valid.
   Calling `setPaths` before `getViewSettings` writes through an invalid
   filesystem pointer; the 00.08 hardware dump caught that failure in
   `glf::Fs::SetDataDir`/`strcpy`.
11. Consume the Java `setViewSettings` callback that reports the requested
    color, depth, and stencil sizes.
12. On surface creation, call `GL2JNILib.stateChanged(true)`.
13. On each surface size change, call `GL2JNILib.resize(width, height)`.
14. On each rendered frame, call `GL2JNILib.step()`.
15. Forward pause/resume through `suspendGame()` and `resumeGame()`.
16. Call `destroy()` when tearing down the activity.

The existing 9mm and MC2 Java tables are structural references because their
packages, classes, and native entry points are different. Their working ports
still establish the shared Gameloft pattern: initialize the loader, JNI and
VitaGL, then reproduce the exact title-specific Java renderer lifecycle.
