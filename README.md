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
    scaling), the GPIO buttons, the vsync line and the 7.37 MHz TCOUNT timer.
  - Wiz/Caanoo: Pollux registers. That includes the MLC layers (same layout as the LF1000 in DidjDroid),
    the DPC vsync flag and the timers.
  - Devices: `/dev/fb0`, `/dev/GPIO`, `/dev/dsp` (OSS), `/dev/mixer`, the F200 touchscreen and the
    Caanoo joystick (`/dev/input/js0`).
- **Timing**: time is virtual and driven by the emulated CPU clock. Programs that busy-wait on vsync are
  fast-forwarded, so a game's speed stays correct even when the phone is slower than the real console.

SDL 1.2 games work through the standard fbcon / OSS / joystick paths. CI builds SDL-style test programs
and checks them.

## Using it

1. Build the APK: GitHub Actions → *Build* → artifact `GP2XDroid-apk`.
2. **Add game folder** (or **Add game .zip**) and pick the game's folder, the one with the `.gpe` and
   its data files.
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
- FPA floating-point instructions (very old hard-float builds). These are reported in the log.
- `fork()`/`system()`.
- YUV video layers, TV out, and networking.
- A JIT. The interpreter runs at about 80–100 MIPS on a desktop, so a lower CPU speed setting can help on phones.
