# NVOF Motion – project status & handoff

> For continuing this work (human or AI): hardware is RTX 4080 + PCSX2 DX12
> + ReShade 6.8.0. No local toolchain anywhere – everything builds in GitHub
> Actions (`build-nvof.yml`, windows-2022, x64), everything is tested on the
> owner's rig (overlay screenshots + `ReShade.log`). Language: Czech.
> Start here, then read `nvof_motion.cpp` (single file, ~1400 lines).

## Goal

HW motion vectors via NVIDIA NVOFA for games/emulators that provide none
(PCSX2). Standalone `nvof_motion.addon64`, no ReShade recompile.
Sibling project (DONE, released): GenericDepthEx – see
`berlint-hub/Depth-buffer_extended` + forum post draft in chat history.
Related prior work: `berlint-hub/PPOM` (finds a game's own velocity
buffer – impossible on PCSX2, PS2-era games have none).

## Architecture (validated)

- Everything bound explicitly at runtime (`GetProcAddress` on `nvcuda.dll`
  + `nvofapi64.dll`); no `.lib` linkage. Silent idle on AMD/Intel/no driver.
- CUDA context LUID-matched to the game D3D12 adapter; own native D3D12
  command list + fence (never touches ReShade's lists).
- Transport: backbuffer (assumed PRESENT at begin_effects) -> CPU readback
  buffer -> fence + zero-timeout poll (never stalls; busy frames skip) ->
  HtoD into NvOF input arrays -> checksum (`Scene activity`). Vectors lag
  ~2 frames by design (history pairing).
- Session builds lazily at real frame size (aligned up to 8), rebuilt on
  resize (`wait_idle` only when live objects exist, never on first build).
- Output: SHORT2 CUARRAY -> host -> CPU int16->float (S10.5, scale 1/32)
  -> half -> R16G16F output texture + `MotionVectors` binding + optional
  injection into configurable `TargetTexture` (default `motion_vectors_tex`).
- Config `[NVOF]`: `Enable=1`, `FlowScale`, `TargetTexture`.
  Grid 4, fast preset, cost plane off.
- SEH guard around bring-up (stage + exception code instead of a crash).
- Zero-copy import (`cuImportExternalMemory`) failed with CU 201 – shelved;
  readback roundtrip (~1-3 ms) is the v1 transport.

## Done & verified on hardware

- 1a: build/load/overlay/`nvofapi64.dll` probe. 1b: ctx/session/caps
  (`32x32..8192x8192`). 2a: capture + checksums, resize rebuild.
- 2b plumbing: PTX kernel exists but is UNUSED (CPU convert instead).
- CI gotchas: `Jimver/cuda-toolkit` needs tag **v0.2.36+** (v0.2.11 does not
  know CUDA 12.6.0) + full semver (`'12.6.0'`, not `'12.6'`). Public NvOF
  headers are Turing-era only (DX12 interface is login-walled); old
  `apiVer` 0x20 works on new drivers. CRLF lesson: normalize to LF.
- Pushing 55KB files by hand risks dropped lines – always byte-verify the
  pushed file against staging + let CI compile. `build_pipeline_locked`
  takes 4 params (`device *dev` first). Overlay build tags (`[roi1]`,
  `[streams]`, `[dali0]`-`[dali19]`, `[d3d12-1]`-`[d3d12-13]`) prove which binary ran – keep tagging test builds.

## CURRENT STATE: OF5 bisection (nvOFExecute = INVALID_PARAM, executes 0)

- Ruled out, in order: CUDEVICEPTR output (CUARRAY still OF5), temporal
  hints off (still OF5), numRois=1 + legal ROI (still OF5, `[roi1]` build),
  SetIO + ROI + hints-off together (still OF5, `[streams]` build 12.10.2026:
  3440x1440, frames 648, executes 0, CU 0 / OF 5), SetIO + zero-init
  (still OF5, `[dali0]` build: frames 3411, executes 0, CU 0 / OF 5).
- FOUND vs OpenCV production code (`opencv_contrib`, same 2.0 headers):
  with external hints OFF they set `hintGridSize = UNDEFINED`, we set 4.
  Also they set `outputCostBuffer = nullptr` explicitly, output dims ceil,
  caps two-stage, and `nvOFGetLastError` for detail (we never called it).
- Current test build `[dali1]` (still OF5, frames 901): SetIO before init +
  zero-init + `hintGridSize = UNDEFINED` + `GetLastError` (= generic
  `Internal execution status`) + real caps (`Grids: 1 2 4 | ROI support: 1`,
  grid 4 confirmed). hintGrid theory ruled out.
- Current test build `[dali2]` (still OF5, frames 3177): SetIO after init
  (OpenCV order) + zero-init + hintGrid UNDEFINED. SetIO order ruled out.
- Current test build `[dali3]` (still OF5, frames 140): as `[dali2]` +
  `disableTemporalHints = TRUE`. Temporal theory ruled out.
- Current test build `[dali4]` (still OF5, frames 132): CUDEVICEPTR +
  stride, ABGR8 kept. Buffer-type theory ruled out.
- Current test build `[dali5]` (still OF5): 128x32 padding (3456x1440)
  did NOT help – alignment ruled out. Debug log proved pitches/handles sane
  (in pitch 13824 = tight, out pitch 3584 = driver-padded, handles alternate
  correctly) and that failure is size-independent (1718x1294 fails too).
- Current test build `[dali6]` (still OF5): input/reference swapped, no
  change – order ruled out (driver can't tell; static scene also proves
  content-independence).
- Current test build `[dali7]` (still OF5): SetIO skipped, no change –
  streams ruled out.
- Current test build `[dali8]` (still OF5): GRAYSCALE8, no change – format
  ruled out. Verified from 2.0 headers: OF 5 = INVALID_PARAM, execute is
  async (NULL-stream ordering is safe via legacy sync), ROI rules confirmed.
- Current test build `[dali9]` (still OF5): temporal on, no change – the
  byte-identical OpenCV recipe fails here, so it is environmental.
- Current test build `[dali10]` (INCONCLUSIVE + new symptom): session builds
  fine on the primary ctx (new 0xC0D... pointers), but per-frame fails with
  CU 201 (INVALID_CONTEXT), memcpys 0 – execute never reached.
- Current test build `[dali11]` (diagnostics): no CTX mismatch lines at build
  (primary attach works), frame fails CU 201 before any CTX check-in – the
  201 comes from frame set-current or the first xfer, not from execute.
  `Scene activity: no` is expected fallout (no upload → no checksum).
- Current test build `[dali12]` (decisive): frame set-current SUCCEEDS and
  GetCurrent matches, yet the first xfer (memcpy) returns CU 201 – the
  retained primary ctx is unusable for direct CUDA calls in this process
  (bring-up NvOF calls work, direct calls don't). Primary hypothesis dead.
- Current test build `[dali13]` (still OF5): SLOW preset, no change –
  preset ruled out.
- Current test build `[dali14]` (still OF5): outGridSize 1, full-res output
  (3456x1440, pitch 13824) – grid ruled out. ALL client-side axes exhausted:
  hints on/off, temporal on/off, ROI on/off, hintGrid 4/UNDEF, SetIO
  before/after/absent, buffers CUARRAY/CUDEVICEPTR, format ABGR8/GRAY8,
  input/ref order, align 8/128, sizes 3440+1718, static/animated content,
  fresh ctx (OF5) vs primary ctx (CU 201 on direct calls), FAST/SLOW,
  grid 4/1. First execute always OF 5 + generic `Internal execution status`.
- Rig (NEMESIS-PC, 13.10.2026): RTX 4080 16GB, driver **617.14** (DCH,
  CUDA 13.4.96, Oct 2026 – very new). 2.0-era execute on this driver is the
  prime regression suspect.
- BREAKTHROUGH (from mbucchia/Optical-Flow-SDK mirror, your link): full
  SDK 5.0 with stock samples. Stock `NvOF::Init` sets
  `NV_OF_INIT_PARAMS::inputBufferFormat` – a field that DOES NOT EXIST in
  2.0. Driver 617.14 (5.x-era) running our apiVer 2.0 session is the prime
  suspect for the deterministic first-execute OF 5.
- Current test build `[dali15]` (still OF5, `api=0x50` confirmed): apiVer
  bump changed nothing – 2.0 and 5.0 paths fail identically.
- Stock defaults verified (`NvOF.h`): temporal ON, cost OFF, hints OFF –
  all identical to ours. The LAST remaining recipe diff: stock
  `Execute(buf[i], buf[i+1])` = input OLDER frame; we had NEWER (dali6+).
- Current test build `[dali16]` (still OF5): stock input order, no change.
- Current test build `[dali17]` (still OF5): exact stock default recipe
  (CUARRAY + MEDIUM + SetIO-nulls) fails identically – params exonerated.
- PROBE VERDICTS: stock recipe PASSES single-threaded AND worker-threaded
  (+8.00px exact, 640x480 + 3440x1440, apiVer 0x50, driver maxApiVer 0x50).
  Driver healthy, threading innocent. Only remaining delta: D3D12-active
  process (PCSX2+ReShade) vs clean probe.
- PIVOT `[d3d12-1]` = native D3D12 NvOF backend (no CUDA at all): session on
  the game device, DEFAULT R8_UNORM inputs + R16G16_SINT grid output,
  UPLOAD/READBACK staging, app/ofa fences (stock AppOFD3D12 discipline:
  upload signals app++, execute waits app + signals ofa++, download waits
  ofa), grid1/MEDIUM/gray/input-older/temporal-on. First run: OF5 with a
  SPECIFIC driver message (`numFencePoints must be atleast 1`) – the D3D12
  path validates and reports, unlike CUDA's generic `Internal execution
  status`.
- Current test build `[d3d12-2]` = `in12.numFencePoints = 1` (single input
  fence, stock `NvOFBatchExecute` passes 1).
- BREAKTHROUGH `[d3d12-2]`: **`first execute OK`, executes 162/163,
  OF 0** on the rig. The missing `numFencePoints` was the only defect.
  CUDA era closed (19 builds, params exonerated, probe keeps passing).
- `[d3d12-3]` production grid4 + FAST executes fine (240/241, OF 0).
- `[d3d12-4]` vector telemetry (grid4/FAST): vectors are LIVE, not frozen
  (`dlhash` differs every 60-frame sample, avg evolves 0->11->28->44).
  Center/origin sane (~0-16px); bottom-right corner wild (~-200,-65px,
  likely border effect + PS2 dither noise in flat areas). The "freeze"
  impression = static scene (stable field is CORRECT) + previously wrong
  `Flow scale` 0.624 instead of true S10.5 0.03125 (20x). With 0.031 the
  user reads avg ~44px / 90% moving – needs scene context to judge
  (fast gameplay = plausible; static menu = noise-driven).
- User reports numbers STATIC during real gameplay motion AND pause, only
  changing on game restart; Enable toggle changes nothing. That is
  non-response, not noise: with temporal hints ON the corner diverges
  (-21 -> -201 over ~180 frames) while center stays ~0 – feedback pathology
  candidate. Stale-upload candidate: post-execute resource states unknown,
  our COMMON->COPY_* barriers could silently no-op after NVOFA touches
  the resources (first upload works, rest frozen at first frames).
- `[d3d12-5]` temporal OFF + synth toggle: static scene reads ~0-1px
  CORRECT, but synth +8px pattern reads ~0.06 (WRONG – probe reads +8.00
  with the identical pattern). Only remaining diffs vs probe for synth:
  temporal OFF (probe ON), grid4 (probe grid1), FAST (probe MEDIUM).
- `[d3d12-6]` temporal back ON: SKIP (user didn't test, superseded).
- **VICTORY `[d3d12-5]` re-enabled: EVERYTHING WORKS.** Temporal OFF is the
  fix – with temporal ON the corner diverges (-21 -> -201) and the field
  goes sluggish/unresponsive; with OFF the vectors track the game.
  (Synth self-test read ~0.06 – pattern-specific FAST/grid4 quirk or mixed
  frames, irrelevant now that game content tracks.)
- Post-mortem freeze saga: user re-tested `[d3d12-3]` (temporal ON) –
  HEALTHY (avg 36.54px / 12.8% localized). So temporal ON works fine;
  the divergence theory is RETRACTED (corner excursions were real
  motion). The "freeze" = ReShade effects toggle OFF (no begin_effects,
  addon idle, overlay stale) + wrong scale 0.624 + static scenes.
  Both ON and OFF track; canonical = ON (stock/guide default, hints help
  video). Synth-0.06 question stays open (pattern too smooth for FAST?),
  minor.
- PHASE 2c DONE: injection into `motion_vectors_tex` works end-to-end,
  debug effect visualizes live flow (user: "BOMBAAAAA JEDEEEEEE").
  Full chain: PCSX2 D3D12 backbuffer -> readback -> luma -> NVOFA (D3D12
  native, grid4/FAST) -> R16G16F field -> effect texture -> shader.
  INT16_MIN invalids filtered, telemetry + self-test + in-menu view stay
  as regression tools.
- Current test build `[d3d12-13]` = QoL batch: STALL watchdog (red warning
  when no frames >2s – the effects-toggle lesson), per-stage ms timings
  (`Times ms: cap/up/exec/down/conv` – answers the GPU-cost question with
  data), debug-log rotation at 2 MB, `Session: WxH, grid GxG` row, persisted
  SynthTest/ShowTex checkboxes, collapsible overlay sections.
- Windowed mode black: debug `.fx` had hardcoded 860x360 (fullscreen
  grid4) but windowed grid is 430x324 – addon correctly skipped the copy
  (dims guard). Fixed with dynamic dims `(BUFFER_WIDTH+3)/4` (same ceil
  formula as the addon grid, matches any resolution).
- NEXT: perf timings per stage in overlay (user asked about GPU cost),
  `paths-ignore` for `*.md`, consumer shaders (frame-gen feed).
- `[d3d12-10]` in-menu view WORKS: live flow field visible (red/green
  directions, black static), executes 10520/10521, avg 12.19px / 94.3%.
  Vectors are real. Overlay "Target: not found" is the INJECTION target
  (needs an effect-side variable) – expected.
- `[d3d12-11]` INT16_MIN invalid-vector filter + `invalid %` row.
- Injection still failing on rig ("Target: not found" with wrong name
  `MotionVectors`; needs exact `motion_vectors_tex`).
- Current test build `[d3d12-12]` = injection diagnostics in debug log
  (`inject 'name': found/injected + var/srv/res handles + actual
  type/fmt/dims vs wanted r16g16_float grid). One run shows exactly which
  check fails (missing variable? binding? dims?).
- PHASE 2c fix: debug `.fx` now DECLARES `texture motion_vectors_tex`
  (860x360 RG16F) and visualizes the INJECTED copy (PPOM pattern:
  addon `find_texture_variable` + copy, dims must match grid). Set addon
  Target texture to exactly `motion_vectors_tex`. Semantic-binding path
  parked (loader needs file unless semantic resolves – unverified).
- Overlay `out grids mask 0x1` legend is misleading (grid 4 provably works).

## Open follow-ups (not blockers)

- Zero-copy import (CU 201) to replace the readback roundtrip.
- `MotionVectors` binding consumer test (QuantMotion/DLSS feed shaders).
- Flow scale 1/32 correctness check against real screen motion.
- `paths-ignore` for `*.md` in workflows (docs-only pushes waste builds).
- Forum post for Depth-buffer_extended release (draft exists in chat).
