# Thunder in Paradise: CD-i static recompilation

A native Windows build of *Thunder in Paradise Interactive* (Philips CD-i,
1995). The game's 68000 code is translated ahead of time into C. A runtime
provides the parts of the player the code expects:

* the CD-RTOS (OS-9) kernel;
* the CD file manager;
* the video decoder (Direct3D 11 presenter at any window size);
* the ADPCM audio processor;
* the **Digital Video (FMV) cartridge** the game requires. It provides the
  MPEG-1 video (`/mv`) and audio (`/ma`) drivers.

The project started from the CD-i recompilation template extracted from the
*Link: The Faces of Evil* port. The recompiled build is bit-identical to a
pure-interpreter run of the same game (`--interp`): frame dumps and audio
compare equal.

## Building

Requirements: Windows 10+, Visual Studio 2022 (C compiler), CMake 3.19+ and
Python 3.8+.

1. Put the disc image in `rom/`:
   `Thunder in Paradise (USA) (Disc 1) (The Game).bin` and `.cue`, with raw
   2352-byte sectors.
2. Run `build.bat`. It extracts the program modules from the disc, recompiles
   them into `gen/`, builds with MSVC and copies `thunder.exe` to the project
   root.
3. Run `thunder.exe`. It looks for the disc image in `rom/` next to the
   program, or you can pass the path of the `.cue` or `.bin`.

Saves (the high score table in emulated NVRAM) and the log go to `saves/`.

## Installing a release

Download from the
[releases page](https://github.com/tools81/thunder-in-paradise-static-recomp/releases):

* **`ThunderInParadise-<version>-setup.exe`**: a single-file installer.
  * It installs per user, without administrator rights, to
    `%LOCALAPPDATA%\Programs\Thunder in Paradise`.
  * It asks for the folder that holds your `.cue`/`.bin`.
  * It adds Start menu and desktop shortcuts.
* **`ThunderInParadise-<version>-portable.zip`**: unzip anywhere and run
  `thunder.exe`. The first time, it asks you to select the `.cue` or `.bin`.

No game files are included: you need your own disc image of *Disc 1 (The
Game)*.

The program is always portable. Everything it writes stays in `saves\` next
to the executable:
* the game's saves;
* quicksaves;
* `settings.ini`;
* `disc_path.txt` (the disc location);
* the log.

Nothing goes to AppData or the registry, apart from the installer's own
uninstall entry, and the shaders are built into the executable. The
shortcuts pass `--portable`; it is accepted but is already the default.

At start-up the program checks this repository's latest GitHub release. If
it's newer than the running version, the program offers to install it. The
`*-portable.zip` is downloaded and unpacked over the program folder, so
`saves\` is kept, and the game restarts. Answering No skips that version.
`--no-update-check` turns the check off; headless and scripted runs never
check.

### Making a release

```
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

This builds the interpreter-only executable (`-DRECOMP_INTERP_ONLY=ON`, in
`build\release`). That executable runs the program from the player's disc
in the 68000 interpreter, so it contains none of the game's code. Its output
is identical to the recompiled build: frame hashes match over 2000 fields.
It runs at about 217 fields per second headless, against 252 for the
recompiled build, which is plenty for 60 Hz plus run-ahead.

The script writes `dist\ThunderInParadise-<version>-portable.zip`. With
Inno Setup 6 (`installer\thunder.iss`), it also writes
`dist\ThunderInParadise-<version>-setup.exe`. The version and the update
repository are set in `runtime/version.h`.

Attach both files to a GitHub release tagged `v<version>`. The in-game
updater looks for the `*-portable.zip` asset of the latest release.

`gen/`, `extracted/` and `rom/` are made from the disc and are not in the
repository. Without `gen/`, CMake builds the interpreter-only version
automatically.

## Controls

| Action | Keyboard | Gamepad (XInput) |
|--------|----------|------------------|
| Move pointer / cursor | Arrow keys (Shift = faster) | Left stick or D-pad |
| Button 1 (action, fire) | Z, Space, A or Enter | A |
| Button 2 | X, Ctrl or S | B or Y |
| Both buttons | C | X or Start |
| Skip the current video | Button 1 | A |
| Fast-forward while held | Tab | |
| Quicksave to the selected slot | F2 | |
| Select the next slot (1–10) | F3 | |
| Load the selected slot | F4 | |
| Invincibility on/off | F5 | |
| FMV upscaler on/off | F8 | |
| Aspect ratio / filter | F9 / F10 | |
| Fullscreen | F11 or Alt+Enter | |
| Quit | Esc | |

On the Controller Selection screen choose **OTHER** for keyboard or gamepad
play, pick a speed, then press **DONE**. The game also offers a light-gun
mode, which takes the pointer position as the aim point.

Command-line options: `--fullscreen`, `--windowed`, `--size WxH`,
`--aspect N`, `--filter N`, `--upscale 0|1`, `--gdi` (software presenter),
`--runahead N` (0–4 fields, default 1) and `--no-smoothing` (no cursor
smoothing).

### FMV upscaling

The full-motion video is upscaled on the GPU from its true resolution with
an edge-directed Lanczos filter, the same one used in the Voyeur port. This
covers the Digital Video cartridge's MPEG picture and any RL7 or DYUV
photographic plane. Contours come out smooth instead of stair-stepped, and
grain stays grain instead of smearing. A structure tensor finds the local
edge direction, the kernel is stretched along edges and kept sharp across
them, and the result is clamped to the nearest source pixels to avoid
ringing.

The interface graphics, the reticle and the on-screen messages keep the
selected filter, so text stays crisp. While composing each line,
`video.c` marks video pixels in the frame's alpha byte. The shader upscales
only those pixels, blending at the edges where video meets graphics.

* The upscaler is on by default with the *sharp* and *smooth* filters.
* Toggle it with F8, `--upscale 0|1`, or `upscale=` in `saves/settings.ini`.
* It needs the Direct3D presenter, so it doesn't work with `--gdi`.
* The CRT and nearest filters show the original pixels.

The disc's data is not changed: the upscaling happens at display time.

Voyeur's AI-upscaled video pack (Real-ESRGAN) is not included. It recognises
pictures by hashing their run-length-coded lines, and Thunder's video is
MPEG from the cartridge instead. A pack for this game would need its own
extractor and a way to identify MPEG frames, such as stream position plus
frame number.

## Quicksave, run-ahead, invincibility and smoothing

* **Snapshots.** The runtime can capture the complete emulated player: the
  68k registers and RAM, every module's globals, and all heap state. That
  includes the MPEG decoders, display tables and sound buffers, which live in
  one arena at a fixed address (`runtime/state.c`). Snapshots are taken
  between fields. `F$Sleep` re-executes instead of waiting inside the call,
  so no host call is ever in progress at that point.
* **Quicksave.** F2 saves to the selected slot, F4 restores it and F3 cycles
  through 10 slots. The selection and results show on screen. Each slot is
  also written to `saves/quicksave_<n>.sav` (about 7 MB) and survives
  restarts. A save only loads into the same build, so files from an older
  build are refused rather than loaded incorrectly.
* **Run-ahead.** Every field, the real field runs first, which keeps the game
  and the audio exactly as normal. Then a snapshot is taken, N more fields run
  with the current input, that picture is shown, and the snapshot is
  restored. The picture is N fields (about 17 ms each) more responsive. This
  costs a second emulation of each field. If it can't keep up with 60 Hz on
  the machine, it switches itself off and says so on screen. It's skipped
  while fast-forwarding.
* **Invincibility (F5)** sets the game's own player-damage switch, `NoDamage`
  (word at a6−28482 in `cdi_Thunder`), on every field. The boat game checks
  it before reducing Thunder's shields, and the island game before applying
  damage to the player. Enemies are unaffected. The game also has its own
  cheat code that sets it. The setting is in `game.json` under `cheats`.
* **Smoothing.** The game moves the hardware cursor, which is the targeting
  reticle in the shooting sections, every second field. It's drawn at 60 Hz,
  gliding from where it was shown to the new position over the usual update
  interval. The software-drawn sprites are a different case. The boat game
  composes its targets and radar into double-buffered pages and flips them
  every 4 fields (15 fps). This is part of its game logic, not a speed limit:
  running the emulated CPU 4× faster still gives 15 fps. Most of the motion
  is animation frames at fixed positions. These pages are shown as the game
  drew them.

## How the FMV cartridge is emulated

The game drives the cartridge as on a real player, so the emulation sits at
the driver level (`runtime/mpeg.c`):

* **Feeding**: `MV_Play`/`MA_Play` give the drivers a chain of PCLs. Either the
  CD file manager fills them during `SS_Play`, with MPEG sectors routed to the
  video and audio channel lists, or the program fills them from memory for
  seamless looping clips. A driver consumes a full PCL, clears it, follows
  `PCL_Nxt` and sends `PCL_Sig`. It stops taking data while its decoder
  buffer is full, unless the CD would otherwise overrun the ring.
* **Decoding**: the sectors carry an MPEG-1 system stream. The driver
  demultiplexes it and decodes with [pl_mpeg](https://github.com/phoboslab/pl_mpeg)
  (MIT licence, vendored as `runtime/pl_mpeg.h`). This is the only
  third-party code in the project. It has one local change, marked in the
  source: MP2 frames may switch between stereo and joint stereo. Upstream
  treats that as lost sync and drops the frames; the intro videos start with
  a joint-stereo frame, so their music played in fragments.
* **Timing**: a 90 kHz system clock starts at the first pack's SCR and runs on
  emulated time. Pictures are presented on their PTS schedule, so the
  program's own timestamp patching for loops (`iso_patch_*`) works as
  intended. Timestamps are compared modulo 2^33: this disc's streams start
  just before the wrap.
* **Events**: picture displayed (0x002), sequence start (0x008) and end of
  play (0x010) are reported through the trigger signal, as
  `(trigger & 0xF800) | events`.
* **Display**: the MPEG picture lies behind both base-case planes, in place of
  the backdrop, so the game's graphics (HUDs, buttons, cursors) overlay the
  video through plane transparency. 352×240 pictures are shown at 2×2 in
  high-resolution coordinates, positioned by `MV_Pos`/`MV_Window`.
* **Audio**: MP2 audio is mixed with the base-case ADPCM output at 44.1 kHz.
  `MA_Cntrl` sets attenuation: byte 3 is L→L, byte 1 is R→R and bytes 2/0
  are the cross paths; 0x80 or more is off.

The full list of function codes is in the header comment of `runtime/mpeg.c`
and in `docs/CDI_NOTES.md`.

## Changes to the template

* `tools/cdi_extract.py` decides that a file is a real-time stream from its
  first sector's submode, not from its file number. Every file on this disc
  has file number 1, including the programs.
* `tools/analyze.py`:
  * reads Microware symbol modules (`<program>.stb`). The disc ships
    `cdi_Thunder.stb`, which supplies all 646 function entries and names.
  * statically resolves calls through Microware's a6 jump table
    (`jsr d16(a6)` into `jmp abs.l` stubs in `_jmptbl`).
  * treats code pointers stored in initialized data (the irefs) as possible
    entries.
  * recognises the `moveq #N,dY / cmp.l dX,dY / bcs` switch bound.
* A jump into recompiled code at an address with no entry point runs in the
  interpreter until execution reaches one. Examples are computed jumps into
  unrolled copy loops, as in `CopyMem` and `QuickBox`. These used to be fatal.
* Kernel:
  * `F$Chain`: the Loader chains to `cdi_Thunder`.
  * `F$SRqMem`/`F$SRqCMem` with size −1 now return the real largest free
    block, capped at 1 MB. They used to return a fixed 256 KB, which starved
    the game's BOLT memory pools.
* CD file manager:
  * a play ends when the program zeroes `PCB_Rec`, which is how the BOLT
    library stops reads.
  * `SS_Pause`/`SS_Cont` are implemented.
* A top-level frontend loop runs one field at a time (`runtime/frontend.c`).
  The scheduler no longer presents frames or polls input from inside the CPU
  loop.
* `F$Time` uses emulated time from a fixed start, so a field always replays
  the same way. NVRAM files are kept in memory and written through to disk.
* Graphics cursor: the SS_GC Blink and Origin codes are 5 and 6, which the
  template had swapped. The cursor pattern is read as 16×16 (one word per
  line) even when the program gives its size in high-resolution units
  (32×32). This made the boat game's targeting reticle appear.
* Audio output waits until about 100 ms is queued before playing, and primes
  again after an underrun. Real-time pacing follows the sound card's queue:
  emulation catches up when the queue runs low. Underruns and fields slower
  than 50 ms are logged to `saves/recomp.log`.
* `disc.c` holds up to 1024 directory entries. It was 64, and this disc has
  202 files.

## Layout

| Path | Contents |
|------|----------|
| `game.json` | Modules to recompile: `cdi_Loader` (boot) at 0xF0000, `cdi_Thunder` at 0x100000 |
| `tools/` | Disc extractor (`cdi_extract.py`), 68000 decoder (`m68k.py`), analyzer (`analyze.py`), C generator (`gen.py`) |
| `runtime/frontend.c` | Top-level field loop: run-ahead, quicksave slots, cheats, on-screen messages |
| `runtime/state.c` | Snapshots: state arena, module state streams, snapshot files |
| `runtime/mpeg.c` | FMV cartridge: `/mv` and `/ma` drivers, demultiplexer, clock, presentation |
| `runtime/pl_mpeg.h` | MPEG-1 video / MP2 audio decoder (third party, MIT) |
| `runtime/os9.c` | OS-9 kernel, including the CSD with the cartridge's devices (types 90 and 91) |
| `runtime/cdfm.c`, `video.c`, `audio.c` | CD file manager, video decoder and compositor, audio processor |
| `runtime/update.c`, `version.h` | Start-up update check against GitHub releases; release version |
| `runtime/find_image.inc` | Disc image search: argument, `saves/disc_path.txt`, `rom/`, first-run file dialog |
| `runtime/interp_only/` | Stand-ins for `gen/` used by the interpreter-only (release) build |
| `installer/`, `tools/package.ps1` | Inno Setup script, player README and third-party notices; release packaging |
| `docs/CDI_NOTES.md` | CD-i, CD-RTOS and Digital Video facts worked out so far |

## Debugging

* `--trace` / `--trace3` log syscalls with callers, signals, disc activity and
  MPEG driver activity. Unimplemented calls are always logged to
  `saves/recomp.log`.
* `--headless --frames N --dump K` runs without a window and writes every Kth
  field as a BMP (`python tools/bmp2png.py file.bmp` converts one). When the
  frame limit is reached, the log shows the 68k pc and a stack backtrace with
  function names.
* `--script "field:key:dur,..."` scripts input. Keys: `1`, `2`, `L R U D`
  (normal speed), `l r u d` (fast), and `Q` / `E` / `W` / `I` for the F2 /
  F3 / F4 / F5 hotkeys.
* `--hashlog` logs a checksum of every presented field (before on-screen
  messages). `--dump-from N` starts frame dumps at field N.
* `--interp` runs everything in the interpreter. Comparing its frame dumps and
  `--wav` output with a recompiled run checks the translation.
* In `--interp` runs, `CDI_BP=addr,addr,...` logs the registers whenever
  execution reaches those addresses. `CDI_BPVARS=-32166,...` adds 16-bit
  variables at those a6 offsets.
* `CDI_PLANES=1` (plane B only) or `CDI_PLANES=2` (plane A only) renders one
  plane ignoring transparency.
* `python tools/analyze.py 1` writes a disassembly of `cdi_Thunder` with
  symbol names to `build/listing_cdi_Thunder.asm`.
* Unit checks: `python tools/gen_tests.py` and `python test/gen_alu_vectors.py`,
  then build and run the `insn_test` and `alu_test` targets.

## Known limitations

* Light-gun play uses the mouse-style pointer as the aim point. Gun
  calibration works but has not been tuned against real hardware.
* `F$GPrDsc` (process descriptor, used by the C library's `clock()`) is not
  implemented and returns an error. The game doesn't appear to depend on it.
* The CD runs at 1× as on the original player, so loads take their original
  time. Hold Tab to fast-forward.
* The game was tested by scripted, headless play through the intro, menus,
  controller set-up, Interactive Television episodes, the boat combat and the
  island arcade sections. A complete playthrough has not been done.
