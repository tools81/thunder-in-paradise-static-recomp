Thunder in Paradise (Philips CD-i) for Windows
==============================================

This is a PC port of the 1995 Philips CD-i game Thunder in Paradise. It runs
the game from YOUR OWN copy of the disc: no game files are included. You
need the game disc ("Disc 1, The Game") as a disc image in .cue/.bin form
(raw 2352-byte sectors).

Getting started
---------------
Installer: run ThunderInParadise-<version>-setup.exe. It asks for the
  folder with your .cue/.bin files, then adds Start menu shortcuts.
Portable zip: unzip ThunderInParadise-<version>-portable.zip anywhere and
  run thunder.exe. The first time, it asks you to select the .cue or .bin.

On the Controller Selection screen choose OTHER for keyboard or gamepad
play, pick a speed, then press DONE.

Everything stays in this folder
-------------------------------
The program writes only next to itself; nothing goes to AppData or the
registry (except the installer's own uninstall entry). Graphics shaders are
built into thunder.exe.
  saves\   the game's own saves (CD-i NVRAM), quicksave_1..10.sav,
           settings.ini, disc_path.txt (where your disc image is),
           recomp.log
To move the game, move the whole folder. (The --portable option in the
shortcuts is the default behaviour and may be left out.)

Controls
--------
Move pointer / aim     arrow keys (Shift = faster), gamepad stick or D-pad
Button 1 (fire)        Z, Space, A, Enter     gamepad A
Button 2               X, Ctrl, S             gamepad B, Y
Both buttons           C                      gamepad X, Start
F2                     quicksave to the selected slot
F3                     select the next slot (1-10)
F4                     load the selected slot
F5                     invincibility on/off (the player only)
F8                     video upscaling on/off
F9 / F10 / F11         aspect ratio / filter / fullscreen (or Alt+Enter)
Tab (hold)             fast forward
Esc                    quit

Quicksaves only load in the version that made them: after an update, older
quicksaves are refused (the game's own saves are kept).

Updates
-------
When the game starts it checks GitHub for a newer release. If there is one,
it asks whether to install it; saves and settings are kept. Answer No to
skip that version. To turn the check off, start the game with
--no-update-check.

Other options
-------------
thunder.exe "path\to\game.cue"   use a disc image without changing the saved one
--runahead N                      run-ahead fields (0-4, default 1)
--upscale 0|1                     video upscaling
--no-smoothing                    no 60 Hz reticle smoothing
--fullscreen, --windowed, --gdi   window options
--version                         print the version

Source and bug reports: https://github.com/tools81/thunder-in-paradise-static-recomp
