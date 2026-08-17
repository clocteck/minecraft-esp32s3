# ClassiCube ESP32-S3 dynamic module

First playable architecture for the Cubic/Holo ESP32-S3 dynamic-module host.
It runs ClassiCube as an app package, like the existing `xianjian` app, instead
of producing a standalone flash image.

## First-version scope

- offline single-player only
- 240x180 3D rendering, enlarged to the 320x240 display (160x120 internal-RAM fallback)
- ClassiCube SoftFP fixed-point software renderer
- RGB565 DMA output to the host display
- game state/options/maps on SD card
- Lua-to-native controller bridge
- PSRAM-backed game heap and task stack
- no networking, music, sound, updater, plugins, or FreeType

The double-buffered 320x240 colour buffers live in PSRAM. The 240x180 Z8 depth
buffer (43,200 bytes) is reserved in internal RAM; allocation failure is reported over serial
and falls back to an internal-RAM 160x120 depth buffer instead of PSRAM. This
build still requires PSRAM for the game heap and intentionally limits the
initial single-player world and view distance.

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
