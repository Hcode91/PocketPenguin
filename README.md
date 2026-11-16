# GP2XDroid

An Android emulator for the GamePark Holdings handhelds: **GP2X F100/F200**, **GP2X Wiz** and **GP2X Caanoo**.

## How it works

All of these consoles run Linux on ARM, and their games are ordinary Linux ARM programs (`.gpe`).
GP2XDroid runs them like `qemu-user` does. It does **not** boot a firmware image:

- **CPU**: the ARM926EJ-S interpreter from DidjDroid. It runs ARMv4T (F100/F200) and ARMv5TE
  (Wiz/Caanoo) code, ARM and Thumb.
- **Kernel**: `core/src/linux.cpp` emulates Linux at the system call level. That covers both the old ABI (OABI,
  `swi 0x9000xx`) used by GP2X-era toolchains and EABI. It handles files, mmap/brk, threads (clone + futex),
  signals, pipes, poll/select, timers and the ARM kuser helpers. It loads static and dynamic ELF programs and
  follows `.gpe`/`.gpu` launcher shell scripts. SD card paths are case-insensitive, like the real FAT card.
- **Hardware** (`core/src/gp2x.cpp`):
  - F100/F200: MMSP2 registers through `/dev/mem`. That includes the MLC display layer (16/8/24 bpp, palette,
    scaling), the display controller, the 2D blitter (raster ops, transparency, patterns), the GPIO buttons,
    the vsync line and the 7.37 MHz TCOUNT timer.
  - Wiz/Caanoo: Pollux registers. That includes the MLC layers (same layout as the LF1000 in DidjDroid),
    the DPC vsync flag and the timers.
  - Devices: `/dev/fb0`, `/dev/GPIO`, `/dev/dsp` (OSS), `/dev/mixer`, the F200 touchscreen, the Wiz/Caanoo
    touchscreen (`/dev/input/event0`, read through tslib) and the Caanoo joystick (`/dev/input/js0`).
- **3D (Wiz/Caanoo)**: calls into the firmware's `libopengles_lite.so` are trapped and run by an OpenGL ES 1.1
  software renderer. That's the same renderer DidjDroid uses, because the Wiz has the same GA3D 3D engine as
  the LeapFrog Didj.
- **FPA floating point**: old toolchains (e.g. the Wiz firmware's glibc) use FPA instructions, which the
  real kernel emulated in software. `core/src/fpa.cpp` does the same.
- **Timing**: time is virtual and driven by the emulated CPU clock. Programs that busy-wait on vsync are
  fast-forwarded, so a game's speed stays correct even when the phone is slower than the real console.

SDL 1.2 games work through the standard fbcon / OSS / joystick paths, and through GP2X "HW SDL" (direct
register access and the blitter). Old LinuxThreads-based programs work too, where each thread acts as its own
process.

Tested with real games:
- **Go Bear Go!** (GP2X, Fenix engine): intro, title, menu and level 1, with music.
- **Deicide 3** (Wiz, dynamically linked against the Wiz firmware 1.2.6 libraries): intro cutscenes, story and
  sound. Its full-motion video is CPU-heavy, because the Wiz ran it at 533 MHz.
- **Propis** (Wiz, OpenGL ES + touchscreen): logo, title, touch menus and music.
- Wiz Toaplan ports (Demons World, Out Zone, Snow Bros 2, Twin Cobra, Zero Wing) boot to their attract screens.

## Using it

1. Build the APK: GitHub Actions → *Build* → artifact `GP2XDroid-apk`.
2. **Add game folder** (or **Add game .zip**) and pick the game's folder, the one with the `.gpe` and
   its data files. **Add SD card .img** imports every game from a whole SD card dump (FAT12/16/32). A `.zip`
   that contains a card `.img` works too. GPH menu `.ini` files are used to pick each game's start file.
3. Tap the game to play. Long-press it to change the console model, the CPU speed or the start file.
   The model is guessed from the program: static old-ABI → GP2X, dynamic old-ABI → Wiz, EABI → Caanoo.
4. Some games are linked against the console's own system libraries. Add those with **Firmware files**:
   a folder or zip with `lib/`, `usr/lib/`, etc. copied from your own device.

Controls: on-screen D-pad, A/B/X/Y, L/R, START/SELECT, VOL−/VOL+, stick push (Caanoo: HOME).
On the F200 model, touching the game screen is the touchscreen. Gamepads work too.
**EMU** → *Show log* shows the game's output and emulator messages, which is useful for bug reports.

## Desktop runner

```
g++ -O2 -std=c++17 -o gp2xcli tools/gp2xcli.cpp core/src/*.cpp
./gp2xcli --model wiz --root FIRMWARE_DIR --mount /mnt/sd=GAMES_DIR -f 600 -o shot.ppm /mnt/sd/game/game.gpe
```
Options: `--strace` logs system calls, `-k FRAME:MASK` presses buttons, `--wav out.wav` saves the audio.
Tests: `tests/run_tests.sh ./gp2xcli` (needs `gcc-arm-linux-gnueabi`).

## Not done yet

- The F100's second CPU (ARM940T), which some emulators use for sound.
- `fork()`/`system()`.
- Games sold through the GPH store (INKA DRM, e.g. Deicide 3 and the Toaplan ports). Their licence sits in a
  protected area of the original SD card that a card dump doesn't contain. The emulator reports this.
- YUV video layers, TV out, and networking.
- A JIT. The interpreter runs at about 80–100 MIPS on a desktop, so a lower CPU speed setting can help on phones.
