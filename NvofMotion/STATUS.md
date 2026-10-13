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
  `[streams]`, `[dali0]`-`[dali6]`) prove which binary ran – keep tagging test builds.

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
- Current test build `[dali6]` = input/reference swapped (OpenCV convention
  `calc(current, previous)`). Debug log stays.
- If `[dali6]` still OF5, next: skip SetIO (driver default streams), then
  `apiVer` (SDK 5.x headers).
- Overlay `out grids mask 0x1` legend is misleading (grid 4 provably works).

## Open follow-ups (not blockers)

- Zero-copy import (CU 201) to replace the readback roundtrip.
- `MotionVectors` binding consumer test (QuantMotion/DLSS feed shaders).
- Flow scale 1/32 correctness check against real screen motion.
- `paths-ignore` for `*.md` in workflows (docs-only pushes waste builds).
- Forum post for Depth-buffer_extended release (draft exists in chat).
