# ClassiCube ESP32-S3 dynamic module

First playable architecture for the Cubic/Holo ESP32-S3 dynamic-module host.
It runs ClassiCube as an app package, like the existing `xianjian` app, instead
of producing a standalone flash image.

## First-version scope

- offline single-player only
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
