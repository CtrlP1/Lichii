# How to run this build

This guide covers The Legend of Zelda: Ocarina of Time (`0400486024DEC000`, game build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`) on Linux with an AMD GPU (RADV). It was tested on one
machine only: i5-12400, RX 7900 XTX, 30 GB of RAM. Other setups may behave differently.

## What you must provide

This repository and its releases contain no game, firmware or keys. Dump all three from a console and
game you own:

- `prod.keys` and `title.keys`
- The Switch firmware
- Your game

## 1. Download

Get the Linux build from the [Releases page](https://github.com/Nolar-Hack/Lichii/releases). The PGO
build is recommended. Make the AppImage executable:

```sh
chmod +x lichii-linux-x86_64-pgo.AppImage
```

## 2. Create a portable folder (recommended)

Lichii uses a folder named `user` in the current directory if it exists, so everything stays together:

```sh
mkdir -p ~/Lichii/user
cd ~/Lichii
```

Start Lichii from this folder. Without a `user` folder, it uses your global configuration.

## 3. Install your keys and firmware

1. **Tools > Install Decryption Keys**, then select the folder containing your keys.
2. **Tools > Install Firmware**, then select your firmware.
3. Restart Lichii.

## 4. Add the game

Add the folder containing your game to the game list, then right-click the game and open
**Properties** for per-game settings.

## 5. Settings used for testing

| Setting | Value |
|---|---|
| GPU Mode | Accurate (tested). Fast has more headroom but was not checked with this build's render fix |
| Asynchronous shader compilation | On |
| Asynchronous presentation | On |
| Console mode | Docked |
| NVDEC emulation | GPU Video Decoding |
| Add-Ons | Dynamic FPS and Max Resolution on (both built in) |
| Frame generation | Off |

## 6. Launch with a steady 30 FPS

Dynamic FPS keeps the game at real speed at any frame rate. This cap keeps frame pacing even, and it
lifts by itself during videos so the intro plays at normal speed. Save this as `lichii.sh` next to the
AppImage:

```sh
#!/usr/bin/env bash
cd "$(dirname "$(readlink -f "$0")")" || exit 1
export EDEN_NX2_SWAP_INTERVAL=2
export EDEN_NX2_PRELOAD_ALL_PIPELINES=1
export EDEN_NX2_IDLE_SPIN_US=0
# Fast GPU mode only: run the GPU on two threads.
export EDEN_NX2_SPLIT=2
# Texture cache sized for a 24 GB GPU. Lower these values on a card with less VRAM.
export EDEN_NX2_TC_EXPECTED_MIB=19456
export EDEN_NX2_TC_CRITICAL_MIB=20480
export EDEN_NX2_TC_MINIMUM_MIB=17408
export EDEN_NX2_TC_MIN_AGE=300
exec ./lichii-linux-x86_64-pgo.AppImage "$@"
```

Remove `EDEN_NX2_SWAP_INTERVAL` to run with a free frame rate.

## What to expect

- Short stutters the first time you see a new area or effect, while its pipelines build. They are
  cached on disk, so close the game normally to keep them.
- Roughly 30 FPS during play on the tested machine.
- This is a very experimental build: expect bugs and crashes.

## Troubleshooting

- **Missing keys or firmware:** repeat step 3 and check that you started Lichii from the same folder.
- **Game too fast or too slow:** keep CPU Clock on Normal and the speed limit at 100%.
- **Very low FPS:** check GPU Mode and your Vulkan driver (Mesa RADV).
