# ClassiCube ESP32-S3 dynamic module

First playable architecture for the Cubic/Holo ESP32-S3 dynamic-module host.
It runs ClassiCube as an app package, like the existing `xianjian` app, instead
of producing a standalone flash image.

## First-version scope

- offline single-player only, with a 160x40x160 generated world
- fixed 192x144 3D rendering enlarged beneath native 320x240 UI
- ClassiCube SoftFP fixed-point software renderer
- RGB565 DMA output to the host display
- game state/options/maps on SD card
- Lua-to-native controller bridge
- Core 0 software-mixed 22050 Hz game sound effects and streamed PCM BGM
- PSRAM-backed game heap and task stack
- no networking, updater, plugins, or FreeType

Two immutable 192x144 RGB565 scene snapshots live in PSRAM. The 192x144 Z8
depth buffer (27,648 bytes) prefers internal RAM and falls back to PSRAM without
changing resolution. During normal gameplay, Core 0 expands six source rows
directly into each 320x10 internal DMA band and overlays a frozen native-resolution
HUD command list. Menus retain one full 320x240 PSRAM compatibility framebuffer.
This build still requires PSRAM for the game heap.

The ESP32-S3 terrain path keeps mesh faces as quads through clipping and
culling, then rasterises textured faces as two affine triangles so all four UV
corners are honoured. Near-to-far face ordering feeds conservative 4x4/8x8 hierarchical
Z rejection for individual quads and whole chunks; a bound is skipped only when
every covered depth cell proves it hidden. Chunk-local bounds use conservative
1/8-block quantisation. The fixed 192-to-320 output map is packed for two-load
RGB565 stores, and an eight-entry tint LUT replaces per-pixel colour multiplies
without changing the resulting RGB565 value. Near-plane raster edge coordinates
use a range that remains safe when the camera is very close to geometry. A
double-buffered temporal Hi-Z cache reuses only exact-view conservative screen
bounds; geometry changes invalidate it. Terrain uses PS1-style affine texture
mapping with full-rate sampling, while held/dynamic models retain their exact
generic path. The ESP32-S3 hot path shares one compact affine triangle kernel
(retaining the quad kernel only for untextured depth passes), advances
interpolation planes by scanline, batches Hi-Z coverage bits,
caches 24 final shaded terrain tiles internally, avoids evicting slots still
referenced by Core 0 raster jobs, and submits batches of 32
complete raster jobs to Core 0.
Distance fog is disabled: the ordered low-cost variant was visibly granular at
192x144, while smooth per-pixel fog consumed too much of the full-frame budget.
The packaged texture set is the complete ClassiCube resource archive with a
256x256 terrain atlas; the earlier N64-only 256x128 atlas omitted upper rows.
Two complete 192x144 PSRAM snapshots provide asynchronous scene ownership. The
mandatory 27,648-byte Z8 buffer gets first claim on internal RAM and cannot fall
back to PSRAM. The colour stage follows: the busier lower 96 rows prefer two
independent 18,432-byte allocations, one for each 48-row band, so they do not
require one contiguous 36 KiB block. If either allocation fails, both are
released before retrying two independent 13,824-byte allocations for the lower
72 rows. The two 320x10 RGB565 DMA blocks (12,800 bytes total) are allocated
last. A failed Z8, colour-layout, or DMA allocation reports an English error,
releases the display resources, and aborts startup. The colour bands are copied
sequentially into the current snapshot only after Core 0 finishes rasterising
it. The Core 0 priority-4
presenter uses a 4 KiB internal stack; the priority-5 raster worker, audio task,
and main task retain PSRAM stacks of 6 KiB, 8 KiB, and 128 KiB respectively.
The optional internal dynamic-vertex arena is capped at 12 KiB.

The ESP32-S3 view-distance option has three fixed tiers: 25, 30, and 35 blocks,
with 30 as the default. The renderer keeps voxel data, resident meshes, and
visible meshes on separate distance budgets. It prebuilds one 16-block chunk
ring beyond the selected visible radius and unloads beyond a second ring. This
prevents rotate-in-place holes without dynamically changing the chosen radius.
Liquid fog may still temporarily reduce the visible radius while submerged.

For repeatable profiling, serial output includes camera position, yaw, pitch,
world mesh vertex count, submitted quads, raster primitives, bounding-box/span
pixels, texture/tint-cache activity, opaque fast-path counts, and perspective
reciprocal counts. Every
display frame clears, transforms, and rasterises a new 3D frame; unchanged
camera views are never reused or skipped. Fixed-seed performance figures must
therefore represent newly generated 3D frames rather than display refreshes.
The native HUD keeps the original FPS/vertex-count/position readout and adds
the fixed view distance; detailed pose and pipeline statistics
remain on serial. Generated still liquids remain static to avoid continuous
background remeshing, while player-placed liquids still flow. Flow levels use
4-bit entries in lazily allocated 8x8x8 pages; still lava encodes source level
in its block type and consumes no level page. A 160x40x160 world therefore has
an 8,000-byte page table instead of a fixed 512,000-byte level array, plus 258
bytes for each page that currently contains flowing lava.

Every selected tier renders a newly generated frame; no unchanged-frame reuse
or static-frame skipping is used. Fixed-tier performance must be measured again
on device after changing the selected distance or presentation path.

Sound effects and `audio/bgm.wav` share one host-owned mono I2S stream. Up to
eight SFX voices are mixed by a PSRAM-stack worker pinned to Core 0. BGM is
stored as 11025 Hz mono 16-bit PCM, read sequentially through an 8 KiB buffer,
and repeated to the 22050 Hz output rate. It consumes no SFX slot, decoder task,
or full-track PCM allocation. UI volume 0-100 maps SFX to 0-200% mixer gain,
while BGM uses one fifth of that curve (BGM 50 is about 20%). The bundled track
loops after a three-second inter-track pause. Two fixed-point peaking biquads boost
roughly 200-500 Hz by 6 dB and trim roughly 1-2.5 kHz by 2 dB on the final Core
0 mix. Both controls are available as controller-friendly
0/25/50/75/100% options.

The bundled default BGM is `Home in the Wilderness` by Herowl, sourced from
VoxeLibre under CC BY-SA 4.0 and converted from stereo OGG to 11025 Hz mono PCM
WAV. See `package/audio/BGM-CREDITS.txt`. A user may replace `audio/bgm.wav`
with another PCM mono 16-bit WAV at 11025 or 22050 Hz.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build-s3-module.ps1
```

PNG screenshots are disabled in production builds. For a debug build, configure
CMake with `-DCLASSICUBE_ENABLE_SCREENSHOTS=ON` before rebuilding. The main game
task retains its 128 KiB PSRAM stack for fast legacy `.dat` loading and the
other large compatibility paths.

Per-quad, clipping, raster-cycle, and detailed work counters are also disabled
in production builds. Configure CMake with
`-DCLASSICUBE_ENABLE_HOT_PROBES=ON` when those intrusive serial probes are
needed. Production output retains frame/stage timing, presenter DMA timing,
memory, map state, and camera pose.

The output module is `package/modules/classicube.so`, and the complete deployable
archive is `dist/classicube-esp32s3-0.1.0.zip`. Copy the complete contents of
`package` to `/sd/apps/classicube` on a compatible current Cubic S3 firmware.
Build success is not device-runtime proof; serial and on-device input, filesystem,
memory, and frame pacing still need to be validated separately.

## Default controls

- left stick / D-pad: move
- right stick: look
- A: jump
- B or SELECT: back/close
- X: inventory
- Y: pick the block being looked at
- RT or L: delete block
- LT or R: place block
- left/right stick click: previous/next hotbar slot
- START: pause; D-pad and A navigate/confirm menus
- HOME: stop the module and leave the app
