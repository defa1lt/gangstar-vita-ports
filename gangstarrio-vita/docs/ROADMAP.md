# Gangstar Rio Vita: completion plan

## Goal and current picture

Bring the Rio port to a reproducible, visually complete, stable and usable
state on a real PS Vita, then produce an installable release that uses the
owner's legally obtained game data. The reported symptoms to reproduce are
missing scene objects/textures, very low frame rate, and audio that has at
times been absent or distorted. A boot/menu screen is not proof that gameplay,
asset streaming, audio or long-session stability work.

The repository already has a verified game profile, APK/ELF preparation,
diagnostic ELF loading, Android import/JNI compatibility work, VitaGL startup,
crash-dump analysis, and a repeatable build/deploy/debug tool. The current
roadmap therefore starts by establishing the exact installed baseline, then
concentrates on the runtime behavior still unproven. These foundations do not
count as acceptance of a playable port.

Work proceeds in five ordered steps. Each step ends with saved evidence and a
clear go/no-go decision; do not tune a later layer to conceal a failure in an
earlier one.

## 1. Reproduce and instrument one exact hardware baseline

**Purpose:** make every subsequent comparison attributable to a particular
build, game-data set, Vita state and reproducible action sequence.

- Record the repository revision, VPK/ELF SHA-256, title ID, Vita firmware and
  plugin configuration relevant to the run, game-data/OBB identity, and whether
  the run is cold or warm. Do not copy game binaries, OBBs, saves, or private
  device details into Git.
- Keep a run record under the ignored `debug/runs/` directory: timestamp,
  build hashes, test scenario, exact failure point, observed audio/video state,
  and paths to the downloaded log, crash dump and screenshots. Keep raw captures
  intact; annotate copies or the run record instead.
- Use the existing persistent `ux0:data/gangstarrio/loader.log` and Vita crash
  dumps as the primary diagnostics. Retrieve the log over Vita Companion FTP
  with `./tools/vita-dev.sh pull` or the direct FTP procedure in
  [Vita FTP debugging](VITA_DEBUGGING.md); download a needed crash dump by its
  exact filename from `ux0:/data/`. Pull evidence immediately after a test,
  before a later launch truncates `loader.log` or replaces a useful dump.
- Take native Vita screenshots at the menu, at the first gameplay frame, and
  wherever an object, texture, or rendering artifact is missing. Pair each
  screenshot with the run record's build hash and action/time; a screenshot
  without build identity is not a regression comparison.
- Use fixed scenarios: clean launch to menu; menu-to-game transition; a short
  on-foot route through the same district; one vehicle route; and one repeat
  after returning to the menu. Note loading duration, stalls, visible assets,
  controls, music and effects separately. Do not infer a cause from a single
  crash, one frame-rate impression, or an unlabelled capture.
- Establish diagnostic overhead before adding instrumentation. Keep high-value
  error, load-stage, memory and watchdog records, but avoid per-frame synchronous
  file writes or verbose hot-path logging. Existing diagnostics batches writes;
  retain that property.

**Exit criteria:** two repeatable runs of the same build and scenario produce
comparable records; the failing transition and at least one missing-asset or
performance symptom are captured with logs/screenshots, or the symptom cannot
be reproduced and the precise test conditions are recorded. The active blocker
is described as an observed stage/symbol/resource, not guessed from the 99%
loading indicator alone.

## 2. Make the rendered scene complete and correct

**Purpose:** fix missing objects, missing textures and visual corruption before
calling a frame-rate improvement successful.

- Trace each failing asset through the actual runtime path:
  game/archive lookup → file read/decompression → format and dimensions → CPU
  image or geometry allocation → VitaGL texture/buffer upload → material and
  shader binding → draw submission. Add identifiers and bounded counters at
  stage boundaries so a missing draw can be distinguished from a missing file,
  failed decode, failed upload, bad material, or omitted scene submission.
- First build an asset inventory from the user's own game data: counts and
  sizes by archive/type, dimensions and mip levels for textures, and the
  formats/features encountered at runtime. Do not mass-convert or recompress
  assets before proving which format, upload path, memory pool, or feature is
  failing. Preserve originals and compare decoded output when testing a
  conversion.
- Audit texture correctness end-to-end: supported compressed formats (including
  ETC variants), mip selection, alpha/transparency, dimensions and alignment,
  row/pitch assumptions, partial/sub-image updates, texture-unit/state changes,
  upload return/error paths, and object lifetime. Check both visible output and
  resource accounting. Keep the existing texture-size/cap experiments bounded
  and evidence-driven; do not turn a temporary cap into an undocumented
  permanent workaround.
- Audit geometry and draw submission independently of textures: vertex/index
  layout and stride, buffer ownership, client-memory lifetime, index type and
  bounds, transforms, culling, depth/blend state, and shader/program setup.
  A model with a valid texture can still be absent because its geometry or draw
  state is invalid.
- Exercise every GLES feature used on the failing path against the VitaGL
  implementation and the game's expectations. Handle unsupported shader paths
  before they reach unsafe VitaGL calls; verify allocation, registration,
  linking, use and destruction order. Do not remove loading progress or suppress
  an error until the work represented by that progress stage has actually
  completed and the missing rendering operation has a verified replacement.
- Compare matched captures from the same camera position and scene. Keep a
  small named set of known-missing objects/textures and verify each one after a
  fix; visual inspection must include the menu, outdoor world, interiors and a
  moving scene, not only a static startup screen.

**Exit criteria:** all objects and texture classes in the chosen representative
scenes render with correct geometry, material, alpha and mip behavior; no known
missing-asset case remains; no new GL errors, out-of-bounds access, or resource
growth is introduced. Save before/after screenshots and the corresponding
asset/load diagnostics.

## 3. Restore complete gameplay systems and lifecycle

**Purpose:** make the correctly rendered game controllable, audible and safe to
enter, leave, save and resume.

- Validate Vita button mapping against the actual Rio UI: menu navigation,
  confirm/cancel (including the observed X-to-enter-menu flow), pause, camera,
  movement, aiming and vehicle controls. Check analog dead zones, repeat rate,
  focus changes and any touch fallback without double-dispatching inputs.
- Trace music and effects separately from their original source to the Vita
  output: asset lookup/decode → game mixer or Android audio API → channel/thread
  scheduling → PCM format/rate/channel count → Vita audio queue → audible output.
  Use short, deterministic checks for menu music, a transition, ambient sound,
  and effects. Record clipping, gaps, wrong rate, noise and channel imbalance;
  do not accept “some sound” as audio parity.
- Confirm audio callback/thread ownership, buffer lifetime, queue depth,
  underflow/overflow behavior, pause/resume, and teardown. Compare the Rio JNI
  contract and data flow with the 9mm hardware reference before borrowing
  anything; transplant only a matching ABI/format/lifecycle behavior, not whole
  files or game-specific identifiers.
- Verify filesystem roots and overlays for the main OBB, patch OBB, loose files,
  settings and saves. Confirm precedence, case sensitivity, short/long reads,
  failed lookups and free-space handling. Preserve existing saves; use a copied
  test save for destructive or migration tests.
- Exercise lifecycle transitions: launch, menu, gameplay, pause/resume, return
  to menu, relaunch, and recovery after a failed load. Check worker-thread
  shutdown, audio/graphics resource release and reload, input state reset, and
  whether a second entry behaves like a clean first entry.

**Exit criteria:** the player can navigate and control the game, menu music and
representative effects are clean and repeatable, data and saves resolve from
the intended locations, and pause/resume/re-entry do not lose input, audio or
graphics state. Attach the run log and screenshots; record the precise audio
path/settings used.

## 4. Measure and improve performance without trading away correctness

**Purpose:** explain the low frame rate with measurements and remove the
dominant costs while retaining the complete scene and useful diagnostics.

- Profile the same fixed scenarios established in step 1, only after step 2's
  scene correctness gate passes. Record frame-time distribution (median, p95
  and worst sustained interval), loading/streaming time, draw counts, CPU-side
  update and submission time, GPU wait/swap behavior where measurable, and
  stalls in file I/O, shader work or synchronization. Compare cold and warm runs.
- Measure memory domains separately: process/heap, decoded CPU assets, staging
  buffers, VitaGL/GPU allocations, texture residency, and peak usage during
  transitions. Track high-water marks and resource counts across repeated
  scene loads; a total-memory number alone cannot identify duplication or leaks.
- Rank changes by measured contribution. Investigate repeated decode/upload,
  redundant copies, avoidable allocations, shader compilation/relinking,
  synchronous I/O, client-array copies, excess state changes/draw calls,
  per-frame diagnostics, and streaming queue starvation. Change one cause at a
  time and rerun the same scenario.
- Consider caching, batching, reduced precision, texture residency policy or
  asset conversion only when the trace shows that work or bandwidth is the
  bottleneck and the memory budget supports the alternative. Validate image
  quality and scene coverage alongside timing. Compression is not a goal by
  itself and is not a substitute for correct asset loading.
- Keep a low-overhead diagnostic mode in the ordinary build: stage timings,
  sampled frame metrics, memory high-water marks, asset failures and crash
  context. Gate expensive traces so instrumentation itself does not recreate
  the reported 10–15 FPS or long-load problem.

**Exit criteria:** a before/after report identifies the dominant bottleneck,
quantifies its change on the same Vita/build conditions, and shows that complete
rendering, stable audio, memory headroom and diagnostic capture remain intact.
Set a practical performance target from the measured baseline and intended Vita
mode before claiming success; do not claim an FPS target from an unmeasured
scene or a single peak reading.

## 5. Run full regression and prepare the installable release

**Purpose:** establish that the integrated build is reproducible and survives
real play, not just a short developer smoke test.

- Test a clean install using the produced VPK and the owner's own prepared game
  data. Verify title ID, LiveArea, permissions, expected directory layout and
  actionable errors for absent/corrupt files. The VPK must not contain the
  copyrighted game binary or game assets.
- Run a coverage matrix across menu, the representative districts, outdoor and
  interior scenes, on-foot play, vehicles, combat, mission/cutscene transitions,
  music/effects, saves, pause/resume and relaunch. Include repeated area changes
  and cold/warm cache cases. Capture screenshots for visual checkpoints and
  keep logs/dumps for failures.
- Run a continuous play soak of at least 30 minutes, then repeat a route with
  several area transitions. Watch for crashes, audio degradation, missing
  objects, sustained memory growth, increasing load times, stuck threads and
  controls that stop responding. Recheck save creation and loading afterward.
- Retain the exact release ELF and symbols for crash analysis; record source
  revision, build command, hashes, required user-provided data layout, tested
  Vita conditions and known limitations. Verify the installed VPK on the Vita,
  not just that a host build completed.
- Keep the test evidence outside source control unless explicitly curated:
  runtime captures belong under ignored `debug/` or `runtime_logs/`. A release
  is not complete if required setup or known-issue documentation is missing.

**Exit criteria:** the clean-install and coverage matrix pass, the 30-minute
soak completes with no crash or sustained memory growth, no known blocker
remains in rendering/audio/controls, and the exact installed release is
identified by hash with its evidence and limitations recorded.

## Decision rules

- Fix correctness before optimizing; fix measured bottlenecks rather than
  guessing; one root cause/change per comparison.
- Keep diagnostic output that answers a concrete failure question, but bound
  frequency, buffering and storage cost. Do not remove logs simply to improve a
  benchmark.
- Treat 9mm and MC2 as references for verified mechanisms only. Rio's JNI,
  assets, game logic and patches are not interchangeable with theirs.
- Preserve original user game data and saves. No bulk recompression, data
  replacement or save deletion as an optimization experiment.
- A milestone passes only with the stated real-device evidence; successful
  compilation, menu entry, process liveness or a one-time screenshot alone is
  insufficient.
