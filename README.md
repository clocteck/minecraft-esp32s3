# ClassiCube ESP32-S3 dynamic module

First playable architecture for the Cubic/Holo ESP32-S3 dynamic-module host.
It runs ClassiCube as an app package, like the existing `xianjian` app, instead
of producing a standalone flash image.

## First-version scope

- offline single-player only, with a 128x40x128 generated world
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
rasterisation. Near-to-far face ordering feeds conservative 4x4/8x8 hierarchical
Z rejection for individual quads and whole chunks; a bound is skipped only when
every covered depth cell proves it hidden. Chunk-local bounds use conservative
1/8-block quantisation. The fixed 192-to-320 output map is packed for two-load
RGB565 stores, and an eight-entry tint LUT replaces per-pixel colour multiplies
without changing the resulting RGB565 value. Near-plane raster edge coordinates
use a range that remains safe when the camera is very close to geometry. A
double-buffered temporal Hi-Z cache reuses only exact-view conservative screen
bounds; geometry changes invalidate it. Terrain uses PS1-style affine texture
mapping with full-rate sampling, while held/dynamic models retain their exact
generic path. The ESP32-S3 hot path shares one compact affine triangle/quad
kernel, advances interpolation planes by scanline, batches Hi-Z coverage bits,
caches 16 final shaded terrain tiles internally, and submits batches of 32
complete raster jobs to Core 0.
Distance fog is disabled: the ordered low-cost variant was visibly granular at
192x144, while smooth per-pixel fog consumed too much of the full-frame budget.
Two 48-row and two 12-row RGB565 colour bands keep 120 of 144 raster rows
(46,080 bytes) in internal RAM; the dynamic-VB arena is 12 KiB, with its colder
tail falling back to PSRAM so the mandatory DMA buffers retain priority.

The user view distance is a maximum of 36 blocks. The renderer keeps voxel data,
resident meshes, and visible meshes on separate distance budgets: one extra
chunk ring is prebuilt, a second ring provides eviction hysteresis, and the
visible radius adapts between 20 and the requested maximum when a complex view
would otherwise fall below the 20 FPS budget. This prevents the old rotate-in-
place holes without silently treating a missing mesh as distance fog.

For repeatable profiling, serial output includes camera position, yaw, pitch,
world mesh vertex count, submitted quads, raster primitives, bounding-box/span
pixels, texture/tint-cache activity, opaque fast-path counts, and perspective
reciprocal counts. Every
display frame clears, transforms, and rasterises a new 3D frame; unchanged
camera views are never reused or skipped. Fixed-seed performance figures must
therefore represent newly generated 3D frames rather than display refreshes.
The native HUD keeps the original FPS/vertex-count/position readout; detailed
pose and pipeline statistics remain on serial. Generated still liquids remain
static to avoid continuous background remeshing, while player-placed liquids
still flow.

At fixed view distance 20, the worst fixed-seed view measures about 36.3-37.0 ms
per complete frame (roughly 27.0-27.4 FPS); simpler directions reach about
31-40 FPS. With maximum view distance 36, open directions retain the full radius
at about 25-32 FPS, while the tested worst view settles at an effective radius
of 26 and about 21.2-21.7 FPS. These are device measurements of newly generated
frames, not display refreshes or unchanged-frame shortcuts.

Sound effects use one host-owned mono I2S stream. Up to eight virtual voices
are mixed by a PSRAM-stack worker pinned to Core 0; sound samples remain in
PSRAM and the host owns all internal DMA buffers. Background music stays
disabled to avoid a second decoder/streaming workload.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build-s3-module.ps1
```

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
