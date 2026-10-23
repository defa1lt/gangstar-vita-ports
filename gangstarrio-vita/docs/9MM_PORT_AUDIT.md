# 9mm Vita solution audit

The local `9mm-vita` port was compared with Rio and the existing MC2
compatibility baseline after the 00.09 hardware crash.

## Shared solutions adopted

- Android kuser helpers: allocate an RX page at `0x9a000000`, install Vita
  implementations for the memory barrier and atomic compare/exchange helpers,
  and rewrite all `0xffff0fa0` and `0xffff0fc0` literals before constructors.
  Rio's pinned binary contains exactly 4 and 43 such literals respectively.
- Loader order: relocate and patch, run constructors, call `gl_preload()`, then
  initialize FalsoJNI and controls before `JNI_OnLoad` and the game lifecycle.
- Performance setup: use the stable 444/222/222/166 MHz ARM, bus, GPU and GPU
  Xbar clocks used by both reference ports.

## Shared solutions already present through MC2

Rio already builds against the local MC2 compatibility layer for Bionic libc,
pthread, IO, EGL, memory mapping, time, OpenSL ES, VitaGL, shaders, touch and
physical controls. The local MC2 implementations contain additional hardening
and tracing absent from the 9mm copies, so replacing them with the older 9mm
files would remove working fixes.

## Title-specific code that cannot be copied

The 9mm runtime patch references 16 symbols in `libCopStory.so` for savegames,
level streaming, zones, checkpoints and Flash HUD controls. An exact dynamic
symbol comparison found zero matches in Rio's `libgl2jni.so`. Its offsets and
ARM instructions therefore do not describe Rio code and must not be applied.

The Java tables and bootstrap also differ. 9mm starts `GLResLoader`, `GLGame`
and `GameRenderer`; Rio uses `GameInstaller` followed by `GL2JNILib`. Rio's
sequence is recovered independently from its APK smali and native disassembly.

Future gameplay-specific patches will be derived from Rio symbols and hardware
dumps, while continuing to use the proven hooking pattern from 9mm.
