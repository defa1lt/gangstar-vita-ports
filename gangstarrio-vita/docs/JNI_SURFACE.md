# Gangstar Rio JNI surface

The supported `libgl2jni.so` performs name-based JNI lookups for Android and
Gameloft helpers. The bridge table in `source/java.c` now covers all 100 real
method names recovered from those lookup sites plus the `Bundle` and
`AudioTrack` constructors. `Heightmap`, `Name`, and `Options` were discarded as
non-JNI strings found next to unrelated native data.

The implementations follow the decompiled Java behavior where it affects the
game: version `1.2.2b`, lower-case manufacturer, ISO-3 locale, stable device
identity, Vita filesystem paths, non-null system strings, and an in-memory
DataSharing map. Android UI/orientation calls are safe no-ops. Bundle accessors
return typed neutral values. The legacy AudioTrack API uses the same Vita PCM
output bridge as the working 9mm port: 44.1 kHz, stereo PCM16, and 4096-byte
grains.

FalsoJNI interns class names for the VM lifetime. Strings and arrays have typed
destructors and counted local/global references; local frames are thread-local.
The loader scopes each controls/step iteration as a native JNI invocation and
releases its temporary references on return. Global references and object-array
elements keep managed objects alive independently of a local frame. Explicit
thread detachment releases that thread's remaining local references. Borrowed
bridge objects, including the AudioTrack placeholder, are never freed as heap
objects. This fixes the persistent FindClass/string-array growth that exhausted
the 224 MiB newlib heap during extended play.

Debug startup exercises the actual JNI table with 10,000 repeated class lookups,
1,000 local frames, global-reference survival, nested-frame result promotion,
object-array replacement and a real pthread detach. `JNI REFS TEST: PASS` requires
balanced managed-object creation/destruction. The watchdog reports the remaining
live count alongside heap usage; sustained gameplay is still required to verify
that all memory growth has stopped.

Run `python3 tools/audit_jni_surface.py` after changing the Java bridge. It
fails when a recovered method is absent or when duplicate name registrations
would be ambiguous in FalsoJNI's name-only resolver.
