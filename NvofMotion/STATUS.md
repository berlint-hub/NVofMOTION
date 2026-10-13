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
  `[streams]`, `[dali0]`-`[dali16]`) prove which binary ran – keep tagging test builds.

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
- Current test build `[dali16]` = input/reference back to stock order
  (input=older). NOTE: flips flow sign vs dali6+ if it ever executes.
- If `[dali16]` still OF5: run stock AppOFCuda on the rig (machine vs code
  verdict) or try another driver (617.14 suspect).
- Overlay `out grids mask 0x1` legend is misleading (grid 4 provably works).

## Open follow-ups (not blockers)

- Zero-copy import (CU 201) to replace the readback roundtrip.
- `MotionVectors` binding consumer test (QuantMotion/DLSS feed shaders).
- Flow scale 1/32 correctness check against real screen motion.
- `paths-ignore` for `*.md` in workflows (docs-only pushes waste builds).
- Forum post for Depth-buffer_extended release (draft exists in chat).
