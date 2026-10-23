# Android source recovery notes

Status: phase 1 started; APK Java recovery completed for the four distinct APK
inputs in `reference/android/apk/`. These inputs and all generated output are
kept out of Git by `/reference/android/` in `.gitignore`.

## Reproducible recovery

JADX 1.5.6 was run from the portable archive in
`reference/android/tooling/jadx-1.5.6/` with two worker threads. Decompiled
sources and resources are in `reference/android/jadx/<variant>/`; captured
logs for the two modified variants are in `reference/android/jadx-logs/`.
Do not treat reconstructed Java as authoritative for a method JADX reports as
failed: use the matching APK's smali/DEX for those methods.

| Variant | Input SHA-256 | Decompiled Java files | DEX files in APK |
| --- | --- | ---: | ---: |
| `clean-1.2.2b` | `203d15fe8f421cbcd1a75012161747abcb9fb6fc197bd4e8504e408aafb28a8c` | 5,688 | 2 |
| `official-GloftG4HM` | `48c008c719d45c7bc61ac8b6dd855707389779158fdce2bf85eac40b04ccab1c` | 5,045 | 1 |
| `modded-orig` | `aae0b5f05343c24bb205126845e8c83f323cdc39b75f8a7b25fb4bc2fc3c5c1a` | 5,689 | 2 |
| `modded` | `8ad35ecddb94d36c6c09376b9c4392514d13a9c0c55fb641bdec57c7a30224d7` | 5,689 | 6 |

`Gangstar_Rio_Signed.apk` was byte-identical to `Gangstar_Rio_Modded.apk`, so
it is represented by the `modded` row rather than decompiled a second time.
The `.orig` file has the same modified native-library hash as `modded`; its
APK SHA-256 is recorded above. Both clean APKs contain the same
`libgl2jni.so` SHA-256 (`c7268efa9cc93da048bc44c5a319531bc6b72668a764b779d17c71bff845cb94`);
both modified APKs contain a different `libgl2jni.so` SHA-256
(`ce03a19f129c2ef4c7310953e65776419a8aa8682d97c6816c551465621117cb`). The
APK's DEX count is taken from the APK contents; JADX may place some DEX files
under `resources/` rather than decompile them into Java.

At DEX level, `modded-orig/classes2.dex` and `modded/classes2.dex` are
byte-identical. The six-DEX modified APK also contains four byte-identical
copies across `classes3.dex` through `classes6.dex`; those four files have
SHA-256 `6a67b5f701670ee9ca45a6e38de68cccbe645bbca46679c11d24bcfb7517d1f8`.
The primary `classes.dex` differs between `.orig` and `modded`, and neither
modified DEX set is byte-identical to the clean APK's two DEX files. Therefore
the modified APK's additional DEX count must not be read as four independent
sets of additional Java code.

JADX exited with code 3 for each variant after writing the decompilation. Its
output reported 8 unreconstructed methods and 56 warnings per input, including
`GameInstaller.d()` and `GMPUtils.a(int,int,Intent)`. This does not invalidate
the remaining output; it marks methods that must be checked against smali.

## Confirmed behavior and differences

- In clean `GL2JNILib.setupPaths()`, the resource root is the value previously
  passed to `setResourcePath`, or `/sdcard/gameloft/games/GloftG4HM` if unset.
  It obtains Android `files` and `cache` directories and calls native
  `setPaths(resource, files, cache)`. This matches the lifecycle already
  recorded in `docs/ANDROID_LIFECYCLE.md`; that document remains the path
  contract for the Vita wrapper.
- Clean `GameInstaller` calls native `nativeStart()` after its licensing
  callback; native startup calls the Java `startGame()` callback. These are
  distinct from the later `GL2JNILib.init()` / `setupPaths()` renderer startup.
- The modified APK adds `System.loadLibrary("bridge")` in both the
  `GloftG4HM` constructor and `GameInstaller` library startup path. The
  constructor also initializes `GamepadProxy`; the activity forces orientation
  value `6` and changes orientation/sensor handling. During activity setup,
  modified code sets `GL2JNILib.f = true` in both branches where clean code
  sets it false in the non-matching branch. This is a concrete behavior delta
  to account for when comparing Android and Vita startup; its impact on the
  missing models/textures is not yet established.
- Modified code removes the clean APK's one-time promotional toast and
  changes several controller/input initialization paths. Those changes are
  not evidence that the modified APK contains a better game-data loader.
- The clean and official-package `GL2JNILib` sources differ in obfuscated
  local/member names. Their path setup and native method contract agree in the
  inspected sections; a full semantic diff is still pending.

## OBB index audit

The existing `reference/android/tools/extract_gameloft_obb.py` parser was used
to read both raw OBB indices and compare them with the already extracted trees.
This is a structural integrity check, not proof that the game requests every
asset in the same way on Android and Vita.

| Archive | Bytes | Indexed entries | Invalid ranges/names | Duplicate paths | Missing extracted files / size mismatches |
| --- | ---: | ---: | ---: | ---: | ---: |
| `main.12030...obb` | 2,119,380,688 | 6,686 | 0 | 0 | 0 / 0 |
| `patch.12221...obb` | 22,356,724 | 371 | 0 | 0 | 0 / 0 |

The main OBB contains 393 `.bdae` model files, 143 `.gmap` files, 11 `.map`
files, 677 `.tga` files, 2,338 `.wav` files and 620 `.glsl` files. The patch
contains no `.bdae` files, one `.gmap`, four `.map`, two `.tga`, seven `.wav`
and 172 `.glsl` files. There are 362 paths shared between main and patch: 56
have different sizes, and another 23 have equal sizes but different SHA-256.
Thus 79 shared paths have different payloads. Seventy are localized strings
or glyphs, two are `.abc` tables, and seven are other files, including
`VOX_SDD.xml`, `m_menu.wav`, `version.txt` and the ETC shader map.

The Android source resolves the precedence question: its ETC profile registers
the patch archive before main; the custom filesystem probes its archive list
from index zero and returns the first match. The Vita implementation likewise
indexes patch before main and returns the first entry. Therefore duplicate
asset precedence matches Android and is not currently an evidence-backed cause
of missing models/textures.

One visual-data exception deserves a targeted follow-up. The shared path
`_packRes/shaders/shadersEtc/GLSL/file.map` differs by one shader define:
main uses `TEXTURE_TYPE 2` for `UnlitTexturedFP.glsl.frag`, while patch uses
`TEXTURE_TYPE 1`. Patch-first selection chooses the patch version, matching
Android registration order. This proves the intended overlay but not whether
Vita's ETC texture upload/decoder semantics match that shader define.

## Vita runtime correlation

The latest available capture is
`debug/runs/20261001-003833-runtime-version-test/loader-current.log`. It logs
both raw archives indexed successfully (patch: 371 entries; main: 6,686).
Later reads include `_packRes/lowmem/city.bdae` and the 2,795,320-byte
`_packEtc/car_cops_texture_damage_d.tga`, so at least these model/texture
payloads resolve through the OBB stream.

The same capture records graphics-task wait #28 lasting 54,624 ms with zero
OBB opens/reads/bytes or counted GL uploads during the wait; the callback
pointer later recorded by the task hook is invoked for 19 ms. Task #48 starts
at 140.439 s and waits on graphics task #28 from 140.754 s; throughout that
interval frame 1853 remains inside the imported native `step()` call. That
boundary is explicit in `source/elf_diagnostics.c`: the frame timer is set
immediately before `step()` and the loop cannot advance until it returns.
Watchdog samples also report the main thread sleeping (`waitType=0x1`) while
the task-run/set counters stay at 48 and the condition counter stays at 10067.
This is evidence of a stalled/sleeping main-thread path, not a frame doing 55
seconds of continuous rendering work. It does not identify which native wait
or dependency holds the thread.

The earlier callback-address audit applied runtime-relative offsets directly
to Ghidra image addresses and was off by `0x10000`. This is visible in the
same clean ELF: its symbol index places `CCpuTask::run` at `0x81dcec` and
`SFunction<SImageLoaderTask>::run` at `0x79cb5c`, while `Global.c` lists them
at `0x82dcec` and `0x7acb5c`. For callback addresses, subtract Vita's module
base `0x98000000` to obtain the ELF address; add `0x10000` only when looking
up that address in the Ghidra export. The clean ELF symbol index therefore
confirms `0x79cb40` as `SFunction<SGenerateMipmaps>::run` and `0x79cb5c` as
`SFunction<SImageLoaderTask>::run`; those two previous labels were valid.
The logged callback `0x9879cae4` is ELF address `0x79cae4`, or Ghidra address
`0x7acae4`, and does not match either callback entry. It is between the listed
`CLinearLookupTableGenerator` destructor entry (`0x7aca94`) and the next
listed run entry (`0x7acb40`); its exact function/object interpretation is
still unresolved. Other hardcoded labels (`0x51ada8`, `0x7ad34c`, `0x883448`,
`0x413e50`) also require rechecking with the corrected bias. An Oct 12 ELF symbol and data
inspection verifies `0x413e48` as `SFunction<SCreateMaterialFunctor>::run`:
the vtable at `0xe3b0e0` yields working vptr `0xe3b0e8`, whose `+8` slot
contains `0x413e48`. Native `CCpuTask::run` reads `task +0x30` and calls that
slot. The 00.79 log independently records vptr `0x98e3b0e8` and main-thread
run `0x98413e48`. This corrected check supersedes the earlier uncertainty
for this material callback; it does not resolve the separate `0x79cae4` case.
A read-only FTP transfer of the current
`ux0:data/gangstarrio/libgl2jni.so` has now independently hashed to the clean
profile SHA-256 `c7268efa…f845cb94`, confirming the currently installed binary.
The Oct 1 capture itself reports only a verification-cache hit, which checks
cached size/mtime rather than hashing during that launch. The current Vita file
and runtime module match the clean-profile input. Separately, logged caller
`0x98779d78` maps at base `0x98000000` to offset `0x779d78`, inside
`ITextureDataLoading::read()` (`Global.c:721436`), which contains no direct
`ITask::wait` call. Therefore the hook's caller field is not yet a trustworthy
direct call-site attribution either. Raw wait/run counters and durations remain
useful observations, but the meaning of `0x79cae4`, the callback object's
vtable, and the wait return address still needs to be reconciled. The
diagnostic code logs callback object/vtable/raw run at wait entry and assigns
names only to the two symbol-verified image/mipmap run entries; it no longer
dereferences a presumed texture from the callback object.

Wait #29 repeats the shape: about 62 seconds, zero OBB/GL counter deltas, then
a 25 ms callback on thread `0x40010003`. Earlier waits #16–27 complete in
54–1,341 ms; several do real OBB reads and texture uploads. This rules out a
general inability to read the OBB or upload every texture. It localizes the
long delay to the interval before the graphics callback is serviced, but does
not distinguish a dependency cycle from another blocked main-thread path.
The 10 ms condition polling in the Vita hook is a correctness hazard, not yet
the demonstrated cause: the wait count is flat during the long interval.

## Oct 2 live reproduction

The diagnostic eboot was uploaded over FTP to the installed app and re-downloaded
for SHA-256 comparison (`285a1e02…f7477be`, matching `build/eboot.bin`). The
previous installed eboot was backed up to
`debug/builds/20261002-180143/installed-eboot-before-upload.bin`. Vita launch
logs and the pre-launch log snapshot are preserved under
`debug/runs/20261002-callback-labels/`.

This run reached native progress 99 at 75.959 s, stage 14 `Managers`; the frame
counter then advanced rapidly. It subsequently entered stage 0
`SceneNodeManager`, so 99% is not the terminal stall. Graphics-task wait #29
began at 132.325 s while frame 1344 was active and completed after 53,582 ms.
The main thread was asleep during the wait, and OBB/GL deltas were zero. The
raw callback address was `0x9879cae4` (`0x79cae4` in the clean ELF). It is not
either verified image-loader or mipmap callback entry, so this run does not
establish that the wait is a texture commit. A second graphics wait
#30 began at 187.224 s and was still active 81,442 ms later, with frame 1346
and main thread asleep. The run was then stopped through Companion after the
logs had been saved. Thus the 99% phase completes, but a repeatable long wait
occurs later during scene-node loading.

There is separate, direct evidence of later allocation failure. The log
repeatedly requests `malloc(2,796,216)` for the same size used shortly before
reading `_packEtc/car_cops_texture_damage_d.tga`; it succeeds at 140.739 s but
returns null at 387.843, 462.106, 540.507, and 621.717 s. That can explain
some missing texture data after those failures, but the log does not yet prove
which failed allocation belongs to which asset request or whether it is the
original cause of the earlier long waits. Treat sampled free-RAM/VRAM values
as supporting clues only: the watchdog reports RAM=0 while nearby task logs
report tens of MB free, so those snapshots are inconsistent and need allocator
and call-site attribution before using them as capacity measurements.

## Oct 2 callback/vtable capture

The next eboot adds callback object, vtable, and raw `run` address to task-run
and wait-entry logs. It was built as `8c0d0927…e3da7dd`, uploaded to
`ux0:/app/GSRV00001/eboot.bin`, and verified by downloading the installed file
over FTP. Its preserved run is `debug/runs/20261002-callback-vtable/`; the app
was stopped through Companion after the capture.

This run again passed native progress 99 at 72.873 s and reached normal frames.
Graphics wait #28 began at 111.339 s and completed after 54,345 ms; the worker
then ran callback `0x9879cae4` for 12 ms. The new fields repeatedly show
callback vtable `0x98e29ce0` and that same raw run address. Neither matches the
symbol-verified mipmap/image-loader entry points, so the callback's concrete
class and purpose remain unknown.

The following stall is a distinct path: after wait #28, frame 553 stayed in
`step()` for at least 56 s while the active wait was `CCondition::wait` on
`0x990330cc`, not `ITask::wait`. During that interval counters stayed at
`waits=28`, `runs=47`, `sets=47`, while condition waits rose from 8,165 to
13,638. Free RAM fell from 25,072 KB to 6,154 KB. At 223.762 s task #48 ran;
task #49 then began and entered graphics wait #29 at 224.549 s using the same
callback vtable/run pair. At the last saved line (244.075 s), wait #29 was
still active at 19.5 s and the main thread was asleep. The app was stopped
before that wait resolved.

The first eight condition-wait entries and every 10,000th entry record caller
offset `0x81e9a8`; this is inside clean ELF `CTaskManager::dispatchTask` at
`0x81e8d8` (Ghidra image coordinate `0x82e8d8`). That ties the second stall
directly to the task-manager dispatcher, rather than merely inferring it from
the condition address.

This confirms two separately observable long waits: a task-completion wait,
then an idle/dispatch condition wait, followed by another task-completion wait.
It makes the condition-variable/queue path a stronger lead than texture upload
time, but does not prove a lost signal or deadlock: no enqueue/signal trace or
native stack was captured. The reduced free-RAM reading also makes allocation
pressure relevant, but no failed allocation appears in this run's saved tail.

Native control flow in `reference/android/reverse/decompiled_code/Global.c`
narrows the next check. `CCpuGraphicsTask::push()` enqueues through the shared
`CTaskManager::push()`; that method takes the manager mutex, links the task,
signals the manager condition, and unlocks (around lines 874332–874351).
`dispatchTask()` takes that mutex, scans pending tasks against the calling
handler's type mask, and waits on the same condition if it has no matching
task (around lines 874456–874492). `CCpuGraphicsTaskHandler::run()` delegates
to the CPU handler and drains pending matching work within its time slice
(around lines 873593–873631).

The Vita `CCondition::wait` replacement in `source/runtime_patch.c` does not
use the condition wrapper's `pthread_cond_wait_soloader`: it unlocks the
mutex, delays 10 ms, then relocks (around lines 456–520). This breaks the
atomic unlock-and-sleep contract of a condition variable and makes a lost
signal possible between unlock and the next predicate check. That is a
concrete concurrency hazard, but the current capture does not prove it caused
the 54–62-second graphics-task delay: while each long `ITask::wait` is active,
the condition-wait counter is flat, and the missing callback must be connected
to the main graphics-handler path before attributing the stall to this hook.
Instrument per-manager enqueue, handler-mask scan, condition wait/signal, and
graphics-handler service thread in one bounded capture before changing the
wait implementation.

The log also records 29 failed `stat` lookups, all absent from both OBB name
indexes (including optional-looking `.pak`/`.bin`, alpha-map and `sculls.tga`
paths). A failed `stat` alone does not prove that a required draw asset failed
to load; trace each request to the consuming call before classifying it as a
defect.

## Next investigation

1. Make the address convention explicit for every artifact: Vita runtime
   pointers are module-base-relative ELF addresses, while `Global.c` uses
   Ghidra's `+0x10000` image coordinates. Use the clean SHA-pinned ELF symbol
   index and matching disassembly to identify `0x79cae4` and vtable
   `0xe29ce0`; do not assign semantic callback names until both resolve.
   Finish method-level APK provenance for `.orig` versus modified
   `classes.dex` and relate modified `libbridge.so` to actual Java/native calls.
2. Map the ETC shader `TEXTURE_TYPE` substitution to Vita texture formats and
   decode/upload handling; verify the 29 missing-stat paths against the actual
   call sites so optional probes are not confused with load failures.
3. Instrument and correlate queued graphics-task enqueue, handler-mask scan,
   condition signal/wait, and service thread around both stall types. Add a
   useful main-thread stack or sampled program counter while frozen; task
   counters and vtable values alone cannot distinguish a lost signal, queue
   starvation, or dependency cycle. Separately trace the null 2,796,216-byte
   allocation to its requesting asset and allocator pool. Correlate both
   findings with missing models/textures before changing rendering, texture
   formats, or compression.

The port change in this phase is diagnostic-only: callback log labels and
texture-specific field reads based on unverified offsets were removed; task
execution is unchanged. `./scripts/build.sh` completed and produced
`build/gangstarrio-vita.vpk`. Its `eboot.bin` was hot-uploaded to the installed
title, verified by FTP hash, launched through Companion, and exercised on Vita.
The run passed 99% but reproduced long scene-load task waits; the VPK itself was
not reinstalled through VitaShell. No graphics or texture behavior was changed.

## Native library decompilation status

Ghidra 12.0.4 headless analysis was run independently against the clean and
modified ARMv7 `libgl2jni.so` files. Inputs are pinned by SHA-256 in the APK
table above (`c7268efa…f845cb94` clean, `ce03a19f…21117cb` modified). Both
analyses found 35,609 function entries. The export produced pseudo-C for 35,251
in each profile; one function at Ghidra address `0x00518818` failed due to an
illegal conditional-execution instruction. The remaining entries are external
import stubs without the Android system libraries needed to decompile them.

Per-function output and the `functions.tsv` status index are in the ignored
local directories `reference/android/reverse/decompiled_clean/` and
`reference/android/reverse/decompiled_modded/`. The matching Ghidra projects are
under `reference/android/reverse/ghidra_headless/RioClean` and `RioModdedFinal`.
`reference/android/reverse/ghidra_scripts/ExportDecompilation.java` is the
reusable exporter. Keeping the script path separate from generated output is
important: Ghidra recursively searches script paths, and pointing it at the
large export tree caused a long delay before the modded export started.

These `.c` files are decompiler pseudo-code, not recovered original source and
not directly buildable. They lack original headers/build configuration and
contain unknown types, guessed names, binary-address globals, and Android/GLES/
OpenSL dependencies. The selected streaming and texture functions were
byte-identical between clean and modded exports at these addresses:
`0x003b7e94` (`IsAllStreamingDone`), `0x003b7ea4` (`PreloadTextures`),
`0x003c7ad0` (`updateStreaming`), `0x003c8160` (`streamingInit`),
`0x003db49c` (`loadSceneNode`), `0x00768558` (`getTextureInternal`),
`0x00774548`, `0x00776258`, and `0x00776cd8` (three implementations named
`loadTextureData`), and `0x0076a520` (`createTextureFromImage`). The function at
`0x00766b44` is an interface stub returning zero; it is not the concrete image
loader. This targeted comparison does not prove the full libraries are
identical, but it shows the modified APK does not contain a fix in these
central texture/streaming paths.

The Vita CMake target compiles `source/*.c` plus loader/compatibility
libraries; it does not compile `reference/android/native_bridge/jni/` or the
decompiler output. The Vita currently loads the original ARM Android `.so`
through the compatibility loader. Therefore a useful editable Vita change must
be reconstructed as a native Vita-compatible implementation/patch in the
actual target, or as a portable source module with explicitly implemented
platform adapters. Rebuilding the original Android `.so` from the Ghidra
exports is not a viable source-recovery path. Use JADX Java/smali and the OBB
extracts for managed code and game data; use Ghidra pseudo-C plus disassembly
as evidence to reconstruct only the native subsystems needed by the port.
