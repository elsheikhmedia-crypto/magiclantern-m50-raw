# Magic Lantern RAW video on the Canon EOS M50 (firmware 1.1.0)

> **EXPERIMENTAL — for developers only.** This is active reverse-engineering
> work. It has hung the camera during testing (a battery pull recovers it).
> There are no ready-to-flash builds. Use it at your own risk.

This branch adds RAW video recording to the Canon EOS M50 on top of
[magiclantern_simplified](https://github.com/reticulatedpines/magiclantern_simplified).
Before this work, the M50 had no RAW video in Magic Lantern.

## What works

- **Continuous MLV RAW** via `mlv_lite` at 10, 12 and 14-bit, using the M50's
  hardware DMA pack unit for 10/12-bit.
- **1920x1080 at 23.976 fps.**
- **Open Gate (3:2 photo-mode sensor readout), 2008x800 at a true 24 fps**:
  32 s / 778 frames with no dropped frames in testing at 10-bit.
- **Correct exposure at 24 fps in photo mode**, done by patching Canon's own
  frame-length table so Canon programs the sensor itself.
- **Shutter-angle control** (90° / 180° / 270° measured 1:2:3).
- Autofocus keeps working during recording.

## In progress

- Taller Open Gate readouts (full sensor height), by editing Canon's
  sensor-mode tables through the MMU.
- Higher-resolution 16:9 / cinema crops.
- Card-speed headroom: the M50 writes about 59 MB/s, close to its ~63 MB/s limit.

## How it was done

The camera ROM was analysed in Ghidra to find:

- Canon's DMA memory-to-memory copy path (`MemoryToMemoryEsub5`), which became
  `platform/M50.110/edmac-d8.c` and the matching stubs.
- The LiveView frame-ready hooks (`EvfState` / `evfReadOutDoneInterrupt`).
- The sensor timing and frame-length tables that control fps and exposure.

The Ghidra helper scripts used for this are in `contrib/m50/ghidra_scripts/`.
The work was done together with Claude (Anthropic's AI assistant).

**No Canon firmware or ROM data is included in this repository.** You need
your own ROM dump from your own camera to reproduce the analysis.

## Where the changes are

| Area | Files |
|---|---|
| M50 platform port | `platform/M50.110/*` (stubs, consts, features, `edmac-d8.c`) |
| RAW pipeline | `src/raw.c`, `src/raw.h`, `src/chdk-dng.c`, `src/edmac-memcpy.c` |
| FPS / timing | `src/fps-engio.c`, `src/state-object.c` |
| Recording | `modules/raw_video/mlv_lite/mlv_lite.c`, `modules/silent/lossless.*` |
| Bring-up tools | `modules/rawdump/` (one-shot LiveView RAW frame dump) |

## Building

Build the same way as upstream magiclantern_simplified for the `M50.110`
platform (`make -C platform/M50.110`).

## License

GPL, the same as Magic Lantern. Original Magic Lantern code is © its
respective authors.
