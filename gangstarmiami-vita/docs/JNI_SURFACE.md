# JNI surface and startup order

Package: `com.gameloft.android.TBFV.GloftGMHP.ML`

## Reconstructed startup

The diagnostic wrapper mirrors Java `onCreate` and
`GameRenderer.onSurfaceCreated`:

1. `Gangster2.nativeSetPhone(960)`
2. `Gangster2.nativeGetInfo("US", deviceId, "Sony Ericsson_R800i", "2.3.4")`
3. `GameRenderer.nativeGetJNIEnv()`
4. `GLResLoader.nativeInit(0)`
5. `GLMediaPlayer.nativeInit(0)`
6. `Gangster2.nativeInit(1)` (the APK's fresh-install demo state)
7. `GameRenderer.nativeInit(1)`
8. `GameRenderer.nativeResize(960, 480)` (default framebuffer scaled to 544)
9. `Gangster2.nativeResume(2, 1)` replaces the focused Activity window.
10. Repeated `GameRenderer.nativeRender()`; after frame 1,
    `Gangster2.nativeResume(2, 0)` restores the internal resume transition.

`isDemo()` returns true when `m_playMode == 0`, the value used by
`loadConfig()` for a fresh install. `nativeGetInfo` is called by the Activity
before surface initialization. The Vita bridge uses the existing R800i
profile and preserves the APK's demo default.

## Native declarations recovered from smali

```text
Gangster2.nativeAccelerometer(FFF)V
Gangster2.nativeCanInterrupt()I
Gangster2.nativeGetInfo(String,String,String,String)V
Gangster2.nativeInit(I)V
Gangster2.nativeIsExit()I
Gangster2.nativeKeyDown(I)V
Gangster2.nativeKeyUp(I)V
Gangster2.nativeOpenIGM()V
Gangster2.nativePause(II)V
Gangster2.nativeResume(II)V
Gangster2.nativeSetPhone(I)V
Gangster2.nativeonTrackballEvent(I)V
GameRenderer.nativeDone()V
GameRenderer.nativeGetJNIEnv()V
GameRenderer.nativeInit(I)V
GameRenderer.nativeRender()V
GameRenderer.nativeResize(II)V
GameGLSurfaceView.nativeOnTouch(IIIJII)V
GLResLoader.nativeInit(I)V
GLMediaPlayer.nativeGetSoundFileName(I)String
GLMediaPlayer.nativeGetTotalSounds()I
GLMediaPlayer.nativeGetTotalSoundsOfSameInstance()I
GLMediaPlayer.nativeInit(I)V
GLMediaPlayer.nativeSetStopOnMusic(I)V
MyVideoView.nativeSetOnVideoCompletion()V
```

`nativeIsExit` appears in smali but is not exported by the supplied ELF. It is
therefore not called by the Vita wrapper and remains a Java/native version
discrepancy to verify later.
