# BytePusher for Adafruit Fruit Jam

This repository contains an original BytePusher virtual-machine implementation for Adafruit Fruit Jam (RP2350B). It is a public-source candidate for the resident-loader application partition, not a standalone full-flash UF2. The user reports that the development port passed basic testing. Its previously accepted development-board baseline averaged about 60 updates/s in recorded warm-run windows. The later HDMI-audio candidate passed software checks and a short sound test, but long-run audio continuity was not established. This Fruit Jam integration has compiled and passed layout checks; it still needs a separate Fruit Jam hardware test. No prebuilt firmware is released here.

The VM implements the [BytePusher specification](https://esolangs.org/wiki/BytePusher): a 24-bit, 16 MiB address space, 65,536 byte-copy-jump instructions per 60 Hz frame, 256x256 indexed video, 16 keys, and 256 signed 8-bit audio samples per frame. A 6 MiB PSRAM page cache backs the virtual memory, with writable state sidecars under `/SAVES/BYTEPUSHER/`. This build uses a 480 MHz RP2350B system-clock profile with conservative flash/PSRAM dividers and an independently checked 126 MHz HDMI clock. This is an overclocked software configuration; its electrical and thermal behavior on Fruit Jam has not yet been measured.

## Programs and input

Put your own lawful `.BytePusher`, `.bytepusher`, or `.bp` program in `/roms/BYTEPUSHER/` on the SD card. No third-party program or saved state is included. The first controller maps BytePusher keys `0..B` to A, Up, X, R, Left, Start, Right, L, Y, Down, Select, B, respectively; keys `C..F` are currently unmapped. Program-specific controls vary.

The repository includes source-only generators for deterministic smoke-test programs. Generate a video/VM test or an audio-bank test locally, then place the resulting file on your own SD card:

```powershell
python ./scripts/make-bytepusher-test-rom.py ./build/BytePusher-Video-Test.BytePusher
python ./scripts/make-bytepusher-audio-test-rom.py ./build/BytePusher-Audio-Test.BytePusher --audio-bank 0x0301
```

Generated program files are ignored by Git and are not part of the repository.

## Build

Use CMake, Ninja, an ARM GCC toolchain, Pico SDK 2.3.0 or newer, and a Pico-PIO-USB checkout:

```powershell
./scripts/build-release.ps1 -PicoSdkPath C:\path\to\pico-sdk -PicoPioUsbPath C:\path\to\Pico-PIO-USB -ToolchainPath C:\path\to\toolchain -NinjaPath C:\path\to\ninja.exe
```

Optional `-PioasmDir`, `-PicotoolDir`, and `-TinyUsbPath` parameters support prebuilt host tools. The output is `build/fruit-jam-release/picoBytePusher.uf2`. The application begins at `0x10080000`; do not flash this UF2 as a standalone bootloader image. Check the layout with:

```powershell
python ./scripts/validate-uf2-layout.py ./build/fruit-jam-release/picoBytePusher.uf2 --app-base 0x10080000 --metadata 0x10FFFF00
```

The release-preparation build with Pico SDK 2.3.0 passed, as did the page-cache model, 100,000-operation value-cache model, and UF2 layout check. These are software checks, not Fruit Jam hardware acceptance.

## Provenance and license

`core/bp_pager.*` and `firmware/bytepusher_main.cpp` implement the public BytePusher specification independently; no third-party emulator core was copied. `firmware/bytepusher_clock13.cpp` provides the 480/126 MHz clock policy; the nonblocking logger and HDMI adapter complete this port. [THIRD_PARTY.md](THIRD_PARTY.md) identifies copied platform components and their licenses. The combined source is distributed under GPLv3, with source-level BSD-3-Clause and upstream notices retained. Building or running this firmware does not grant rights to redistribute third-party BytePusher programs.
