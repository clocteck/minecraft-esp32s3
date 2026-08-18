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
- Core 0 software-mixed 22050 Hz game sound effects through the host audio API
- PSRAM-backed game heap and task stack
- no networking, music, updater, plugins, or FreeType

The double-buffered 320x240 colour buffers live in PSRAM. The 192x144 Z8 depth
buffer (27,648 bytes) prefers internal RAM and falls back to PSRAM without changing
resolution. 3D samples use fixed 5:3 expansion into the native 320x240 framebuffer while
UI remains native. This build still requires PSRAM for the game heap.

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
The 27,648-byte Z8 buffer gets first claim on internal RAM after the mandatory
LCD DMA blocks. The 192x144 RGB565 3D stage is split evenly: its top 72 rows use
27,648 bytes of PSRAM, while its busier lower 72 rows use two 36-row internal
bands totalling 27,648 bytes. LCD output uses two 320x10 RGB565 DMA blocks
(12,800 bytes total). The Core 0 priority-4 presenter uses a 4 KiB internal
stack; the main, raster, and audio task stacks remain in PSRAM at 128 KiB,
6 KiB, and 6 KiB respectively. The optional internal dynamic-vertex arena is
capped at 8 KiB.

The user view distance is a maximum of 32 blocks. The renderer keeps voxel data,
resident meshes, and visible meshes on separate distance budgets: at the default
maximum it prebuilds through 48 blocks and unloads beyond 64 blocks. The visible
radius adapts between 20 and the requested maximum. Two valid frames
below 9 FPS reduce it by 2, eight valid frames below 12 FPS reduce it by 1,
and twenty valid frames above 16 FPS increase it by 1. Chunk-build frames and
long stalls are excluded from these counters. This prevents the old rotate-in-
place holes without silently treating a missing mesh as distance fog.
Liquid fog may temporarily reduce the visible radius below 20; leaving the
liquid restores it immediately to 20 before normal adaptation resumes.

For repeatable profiling, serial output includes camera position, yaw, pitch,
world mesh vertex count, submitted quads, raster primitives, bounding-box/span
pixels, texture/tint-cache activity, opaque fast-path counts, and perspective
reciprocal counts. Every
display frame clears, transforms, and rasterises a new 3D frame; unchanged
camera views are never reused or skipped. Fixed-seed performance figures must
therefore represent newly generated 3D frames rather than display refreshes.
The native HUD keeps the original FPS/vertex-count/position readout and adds
the effective/configured view distance; detailed pose and pipeline statistics
remain on serial. Generated still liquids remain static to avoid continuous
background remeshing, while player-placed liquids still flow. Flow levels use
4-bit entries in lazily allocated 8x8x8 pages; still lava encodes source level
in its block type and consumes no level page. A 160x40x160 world therefore has
an 8,000-byte page table instead of a fixed 512,000-byte level array, plus 258
bytes for each page that currently contains flowing lava.

At fixed view distance 20, the worst fixed-seed view measures about 36.3-37.0 ms
per complete frame (roughly 27.0-27.4 FPS); simpler directions reach about
31-40 FPS. The adaptive 20-32 range trades available frame-time headroom for
distance according to the thresholds above. These are device measurements of
newly generated frames, not display refreshes or unchanged-frame shortcuts.

Sound effects use one host-owned mono I2S stream. Up to eight virtual voices
are mixed by a PSRAM-stack worker pinned to Core 0; sound samples remain in
PSRAM and the host owns all internal DMA buffers. Background music stays
disabled to avoid a second decoder/streaming workload.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build-s3-module.ps1
```

PNG screenshots are disabled in production builds. For a debug build, configure
CMake with `-DCLASSICUBE_ENABLE_SCREENSHOTS=ON` before rebuilding. The main game
task retains its 128 KiB PSRAM stack for fast legacy `.dat` loading and the
other large compatibility paths.

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
