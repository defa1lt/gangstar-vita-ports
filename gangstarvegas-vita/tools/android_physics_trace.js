'use strict';
// The builder validates the native SHA and prepends GAME_TRACE_PROFILE.
const output = '/sdcard/Android/data/com.gameloft.android.ANMP.GloftGGHM/files/vegas-physics-trace/';
const limit = 8 * 1024 * 1024, duration = 300000;
const counts = {}, stepResults = {}, listeners = [];
let log, image, observer, timer, flusher, stopped = false, scheduled = false;
let bytes = 0, sequence = 0, steps = 0, lastSnapshot = 0, maxDelta = 0, anchor = null, ioFailed = false;
const started = Date.now();
const queryNames = new Set(['get_player', 'get_position', 'get_physics_body', 'get_hkp_body', 'streamer_done', 'streamer_district_loaded']);
let query, xyz, anchorBuffer;

function utf8Size(text) {
  let size = 0;
  for (const c of text) { const n = c.codePointAt(0); size += n < 128 ? 1 : n < 2048 ? 2 : n < 65536 ? 3 : 4; }
  return size;
}
function ioFailure(error) {
  if (ioFailed) return;
  ioFailed = true; scheduled = true;
  console.error(JSON.stringify({event: 'physics_trace_io_error', message: String(error)}));
  clearTimeout(timer); clearInterval(flusher);
  if (!stopped) setTimeout(() => stop('io_error'), 0);
}
function emit(event, fields = {}, final = false) {
  if (!log || ioFailed || (stopped && !final) || (scheduled && !final)) return;
  let line = JSON.stringify({event, ms: Date.now() - started, sequence: sequence++, ...fields}) + '\n';
  let size = utf8Size(line);
  if (!final && bytes + size > limit - 2048) {
    scheduled = true; setTimeout(() => stop('byte_limit'), 0); return;
  }
  if (final && bytes + size > limit) {
    line = JSON.stringify({event: 'trace_end', reason: fields.reason, omitted_summary: true}) + '\n';
    size = utf8Size(line);
  }
  if (bytes + size > limit) { ioFailure(new Error('No room for final trace record')); return; }
  try { log.write(line); bytes += size; } catch (error) { ioFailure(error); }
}
function stop(reason) {
  if (stopped) return;
  stopped = true;
  clearTimeout(timer); clearInterval(flusher);
  try {
    if (observer) { try { observer.detach(); } catch (error) { console.error(String(error)); } }
    for (const listener of listeners) { try { listener.detach(); } catch (error) { console.error(String(error)); } }
    emit('trace_end', {reason, bytes_before_end: bytes, counts, steps, step_results: stepResults, max_delta: maxDelta}, true);
  } finally {
    if (log) {
      try { log.flush(); } catch (error) { ioFailure(error); }
      try { log.close(); } catch (error) { ioFailure(error); }
      log = null;
    }
  }
}
function safely(fn) { try { return fn(); } catch (error) { return {read_error: String(error)}; } }
function pointer(p, offset) { return p.add(offset).readPointer(); }
function text(p) {
  if (p.isNull()) return null;
  return safely(() => {
    const range = Process.findRangeByAddress(p);
    if (!range || !range.protection.includes('r')) throw new Error('Unreadable path');
    const size = Math.min(512, range.base.add(range.size).sub(p).toUInt32());
    const raw = new Uint8Array(p.readByteArray(size)), end = raw.indexOf(0);
    if (end < 0) throw new Error('Unterminated path within 512 bytes');
    return end === 0 ? '' : p.readUtf8String(end);
  });
}
function floatBits(p) {
  const b = new ArrayBuffer(4), d = new DataView(b);
  d.setUint32(0, p.toUInt32(), true); return d.getFloat32(0, true);
}
function entity(p) {
  const f = GAME_TRACE_PROFILE.physics;
  if (p.isNull()) return null;
  return safely(() => ({body: p.toString(), world: pointer(p, f.body_world).toString(),
    shape: pointer(p, f.body_shape).toString(), island: pointer(p, f.body_island).toString(),
    userdata: pointer(p, f.body_userdata).toString(), motion_type: p.add(f.body_motion_type).readU8()}));
}
function snapshot() {
  const f = GAME_TRACE_PROFILE.physics, p = f.player;
  const world = image.base.add(f.world_slot).readPointer();
  const state = {frame: steps, world: world.toString(), counts: {...counts}, max_delta: maxDelta, step_results: {...stepResults}};
  if (world.isNull()) { emit('physics_state', state); return; }
  const havok = pointer(world, f.world_havok), streamer = pointer(world, f.world_streamer);
  state.havok_world = havok.toString(); state.streamer = streamer.toString();
  const begin = pointer(world, f.world_body_begin), end = pointer(world, f.world_body_end);
  const span = end.sub(begin).toInt32();
  state.native_body_count = end.compare(begin) >= 0 && span >= 0 && span % 4 === 0 ? span / 4 : null;
  if (!havok.isNull()) {
    const fixed = pointer(havok, f.fixed_island);
    state.fixed_island = fixed.toString();
    state.fixed_entity_count = fixed.isNull() ? null : fixed.add(f.island_entity_count).readS32();
  }
  const grid = streamer.isNull() ? ptr(0) : pointer(streamer, f.streamer_grid);
  state.grid = grid.toString();
  if (!grid.isNull()) state.streaming_done = query.streamer_done(streamer) !== 0;
  const player = query.get_player(); state.player = player.toString();
  if (!player.isNull() && player.readPointer().equals(image.base.add(p.vptr))) {
    const node = pointer(player, p.node), rawPhysics = pointer(player, p.physics_reference);
    const physics = query.get_physics_body(player);
    state.player_hide = player.add(p.flags).readU32() & 0x400;
    state.player_fall = player.add(p.fall_state).readS32();
    state.player_physics_reference = rawPhysics.toString();
    state.player_physics_body = physics.toString(); state.node = node.toString();
    if (!node.isNull()) {
      const result = query.get_position(xyz, player);
      if (!result.equals(xyz)) throw new Error('GetPosition output ABI mismatch');
      const position = [0, 4, 8].map(n => xyz.add(n).readFloat());
      if (position.every(Number.isFinite)) {
        state.position = position;
        if (!anchor || Math.hypot(position[0] - anchor[0], position[1] - anchor[1]) > 1000) {
          anchor = position.slice();
          for (let i = 0; i < 3; i++) anchorBuffer.add(i * 4).writeFloat(anchor[i]);
          emit('district_anchor', {position: anchor, frame: steps});
        }
        state.anchor_position = anchor;
        if (!grid.isNull()) {
          state.district_current = query.streamer_district_loaded(streamer, xyz) !== 0;
          state.district_anchor = query.streamer_district_loaded(streamer, anchorBuffer) !== 0;
        }
      }
    }
    if (!physics.isNull()) {
      const body = query.get_hkp_body(world, physics);
      state.player_body = entity(body);
      state.player_body_matches_world = !body.isNull() && pointer(body, f.body_world).equals(havok);
    }
  }
  emit('physics_state', state);
}
function install(module) {
  if (image || stopped) return;
  image = module;
  if (Process.arch !== 'arm' || Process.pointerSize !== 4) throw new Error('ARM32 required');
  const f = GAME_TRACE_PROFILE.physics, functions = {};
  // Validate every address before attaching any hook or making a native call.
  for (const entry of f.functions) {
    const raw = new Uint8Array(image.base.add(entry.offset).readByteArray(8));
    const actual = Array.from(raw, n => n.toString(16).padStart(2, '0')).join('');
    if (actual !== entry.guard) throw new Error('Runtime guard mismatch: ' + entry.name);
    functions[entry.name] = image.base.add(entry.offset);
  }
  const jni = GAME_TRACE_PROFILE.jni.filter(e => e.name.endsWith('GL2JNILib_step'));
  if (jni.length !== 1) throw new Error('Expected one JNI step entry');
  query = {
    get_player: new NativeFunction(functions.get_player, 'pointer', []),
    get_position: new NativeFunction(functions.get_position, 'pointer', ['pointer', 'pointer']),
    get_physics_body: new NativeFunction(functions.get_physics_body, 'pointer', ['pointer']),
    get_hkp_body: new NativeFunction(functions.get_hkp_body, 'pointer', ['pointer', 'pointer']),
    streamer_done: new NativeFunction(functions.streamer_done, 'int', ['pointer']),
    streamer_district_loaded: new NativeFunction(functions.streamer_district_loaded, 'int', ['pointer', 'pointer'])
  };
  xyz = Memory.alloc(12); anchorBuffer = Memory.alloc(12);
  for (const entry of f.functions) {
    if (queryNames.has(entry.name)) continue;
    listeners.push(Interceptor.attach(functions[entry.name], {
      onEnter(args) {
        if (stopped || scheduled) return;
        this.active = true; this.begin = Date.now();
        const count = counts[entry.name] = (counts[entry.name] || 0) + 1;
        this.sample = count <= 64 || (count & (count - 1)) === 0;
        this.data = {name: entry.name, count, tid: Process.getCurrentThreadId(),
          caller: this.returnAddress.sub(image.base).toString(), args: [args[0], args[1], args[2], args[3]].map(String)};
        if (entry.result.startsWith('file_')) this.data.path = text(args[1]);
        if (entry.name === 'hkp_add_entity') this.body = args[1];
        if (entry.name === 'hkp_add_batch') this.data.batch_count = args[2].toInt32();
        if (entry.name === 'hkp_step' || entry.name.startsWith('world_update')) {
          this.data.delta = floatBits(args[1]);
          if (entry.name !== 'hkp_step') this.data.enabled = args[2].toUInt32();
          if (entry.name === 'hkp_step' && Number.isFinite(this.data.delta)) maxDelta = Math.max(maxDelta, this.data.delta);
        }
        if (this.sample) emit('native_enter', this.data);
      },
      onLeave(result) {
        if (!this.active || stopped || scheduled) return;
        if (entry.name === 'hkp_step') {
          let key = String(result.toInt32());
          if (!(key in stepResults) && Object.keys(stepResults).length >= 15) key = 'other';
          stepResults[key] = (stepResults[key] || 0) + 1;
        }
        if (!this.sample && !(entry.name === 'hkp_step' && result.toInt32() !== 0)) return;
        const data = {...this.data, elapsed_ms: Date.now() - this.begin};
        if (entry.result !== 'void' && entry.result !== 'file_void') data.result = result.toString();
        if (this.body) data.entity_after = entity(this.body);
        emit('native_leave', data);
      }
    }));
  }
  listeners.push(Interceptor.attach(image.base.add(jni[0].offset), {
    onLeave() {
      if (stopped || scheduled) return;
      steps++;
      if (Date.now() - lastSnapshot < 1000) return;
      lastSnapshot = Date.now();
      try { snapshot(); } catch (error) { emit('snapshot_error', {message: String(error), frame: steps}); }
    }
  }));
  emit('hooks_installed', {native: listeners.length - 1, jni: jni[0].name, module_base: image.base.toString(),
    module_size: image.size, native_sha256: GAME_TRACE_PROFILE.native_sha256, text_sha256: GAME_TRACE_PROFILE.text_sha256});
}
rpc.exports = {
  init() {
    try {
      const libc = Process.getModuleByName('libc.so');
      const mkdir = new NativeFunction(libc.getExportByName('mkdir'), 'int', ['pointer', 'int']);
      mkdir(Memory.allocUtf8String(output), 0x1c0);
      log = new File(output + 'events-' + Date.now() + '.jsonl', 'w');
      emit('trace_start', {mode: 'physics', frida: Frida.version, arch: Process.arch, pid: Process.id,
        duration_ms: duration, max_bytes: limit, coverage: 'guarded physics hooks and read-only post-JNI-step state; sampled calls and all-call counters'});
      timer = setTimeout(() => stop('time_limit'), duration);
      flusher = setInterval(() => { if (log) { try { log.flush(); } catch (error) { ioFailure(error); } } }, 1000);
      observer = Process.attachModuleObserver({onAdded(module) {
        if (module.name !== 'libGangstar4.so') return;
        try { install(module); }
        catch (error) { emit('trace_error', {message: String(error)}); setTimeout(() => stop('profile_error'), 0); }
      }});
    } catch (error) {
      console.error(JSON.stringify({event: 'physics_trace_initialization_error', message: String(error)}));
      emit('trace_error', {message: String(error)}); stop('initialization_error');
    }
  },
  dispose() { stop('dispose'); }
};
