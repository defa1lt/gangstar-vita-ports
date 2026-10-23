# MC2BPegasus-Vita baseline

The local project `/Volumes/USB/persanal/psvita/MC2BPegasus-Vita` was audited
as a second loader reference.

```text
repository: https://github.com/WolffsRoom/MC2BPegasus-Vita.git
commit:     253676d4e0a3c33cfab133a06c3c1e6ad552a26f
license:    MIT
```

It is a stronger compatibility-layer baseline for Gangstar Rio than the
initial 9mm comparison. Its `source/dynlib.c` contains literal mappings for 353
of Rio's 365 undefined dynamic symbols. The earlier 9mm-vita comparison matched
349 names.

The 12 names still absent from the MC2 mapping are:

```text
closelog
getegid
geteuid
getgid
getpwuid
getuid
openlog
pread
statfs
syslog
zError
zlibVersion
```

They are implemented by `source/rio_missing_imports.c`. The combined MC2 and
Rio tables therefore cover all 365 imports in the supported game binary. They
are kept separate so MC2 can remain a pinned, auditable upstream reference.

MC2 already provides useful working implementations or integration patterns
for:

- `so_util`, kubridge and FalsoJNI;
- VitaGL setup and shader preloading;
- Bionic libc, pthread, errno, filesystem and time compatibility;
- all four Rio 3D texture imports;
- OpenSL ES and Java-mediated audio;
- Xperia Play physical controls and touch input;
- persistent loader tracing and data-file preflight.

The MC2 game lifecycle and Java table cannot be reused unchanged. MC2 calls
`Game_nativeInit` and `GameRenderer_nativeInit/nativeRender`; Rio exports
`GL2JNILib_init`, `setPaths`, `initGL`, `resize`, `step`, and `touchEvent`.
MC2-specific offsets, patches, paths, audio names and JNI method IDs must be
removed or re-derived before executing Rio.

The implementation order is therefore:

1. retain the M0 binary verifier in this repository;
2. import generic loader/reimplementation code from the pinned MC2 commit with
   its copyright headers and license;
3. add the 12 missing system mappings;
4. stop before `so_initialize` until runtime import diagnostics are clean;
5. replace MC2 JNI and main lifecycle with the sequence recovered from Rio
   smali;
6. adapt path translation for `GloftG4HM` and its proprietary OBB containers.
