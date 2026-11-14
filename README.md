# Midnight Club 3: DUB Edition Remix on Xbox 360

My native port of Midnight Club 3: DUB Edition Remix (original Xbox, USA version) for RGH/JTAG Xbox 360 consoles. [Leia em português](LEIAME.md).

It's not an emulator and it's not backward compatibility. The game's x86 code is translated to C++ on a PC and compiled for the 360's PowerPC CPU, with the Xbox kernel, Direct3D 8 and DirectSound rebuilt on top of the 360's own libraries. The result is a real `default.xex`, built from your own copy of the game.

**No game here.** This repository has no game files, no prebuilt XEX and no SDK. You need your own copy of the game, and you have to install the tools yourself. The generated code and the XEX contain the game's code, so please don't share them.

## Where it stands

- Boots to the menus, career, free roam and races. Music, engine sounds, the HUD, the minimap, the speed blur and the headlights on the road all work.
- The original 720x480 picture is drawn at full quality and shown at 1280x720, widescreen.
- Race time follows real time: when a frame takes longer, the game advances its own steps to keep up, so cars and the race clock don't slow down.
- Performance on a real console is in the table below.
- The intro videos at boot (Rockstar, Rockstar San Diego and the game's opening) play, and so do the tutorial videos.
- I haven't tested the whole career, saving and loading, or replays on the console yet.

## Performance

Measured on my RGH console, 720p, with the default `trafego.txt` and `lod.txt`. The frame rate depends mostly on how much of the city is on screen, so it changes a lot along the same track.

| Where | FPS |
|---|---|
| Menus and boot screens | about 22 |
| Light streets (open areas, little around) | 19 to 26 |
| Medium streets | 16 to 18 |
| Heavy streets (downtown, many buildings and traffic) | 14 to 15 |

The game was made for 30 fps on the original Xbox. Here the CPU is the limit: the translated x86 code runs slower on the 360's PowerPC than the original did on its own CPU. Race time follows real time anyway, so a lower frame rate doesn't slow the race down.

## What you need

- An Xbox 360 with RGH or JTAG.
- Your own copy of the game, USA version, extracted from the disc: `default.xbe` and all the other files and folders next to it. The XBE's SHA-1 must be `5bca1cf092edaadf876e8f80129900407a4a07b8`.
- Windows, 64-bit. I build on Windows 11.
- **Visual Studio 2010**. The SDK needs it. I use the Ultimate edition.
- **Xbox 360 SDK 2.0.21256** (XDK), installed after VS 2010. Its installer sets the `XEDK` variable. Check it with `echo %XEDK%`.
- **Visual Studio 2022 Build Tools**, with the "Desktop development with C++" workload. It brings MSVC, CMake and Ninja, used for the PC-side tools.
- **Git**. CMake uses it to download Zydis and toml++.
- **Python 3**. No extra packages.

## Building

1. Copy your `default.xbe` to `pc-recomp\assets\default.xbe`.

2. Build the PC tools (the XBE analyzer and the recompiler). Run `tools_build.bat`, or open the **x64 Native Tools Command Prompt for VS 2022** in the project folder and run:

   ```
   cmake -S x360/tools -B x360/tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build x360/tools/build
   ```

3. Compile the runtime (the kernel, Direct3D, audio and the game's fixes) for the 360:

   ```
   powershell -ExecutionPolicy Bypass -File x360\build_runtime.ps1
   ```

4. Translate the game, compile it and link the XEX:

   ```
   python build_all.py
   ```

   This creates `pc-recomp\generated` from your XBE, compiles it into `x360\build\gen` and links `x360\build\default.xex`. If the VS 2010 compiler hits an internal error on some function, or the linker fails on a file during link-time optimization, the script sets it aside and tries again. On my PC the whole build takes about 15 minutes, most of it in the link.

   To link again after changing only the runtime, run `x360\build_runtime.ps1` and then `x360\build_xex.ps1`.

## Installing on the console

1. Create a folder on the HDD or a USB drive and copy every file and folder from the game disc into it, `default.xbe` included.
2. Put `default.xex` in the same folder. The XBE has to stay there, because the port reads its data.
3. Launch `default.xex` from Aurora, FreeStyle Dash or XeXMenu.
4. Saves go to `hdd` inside the game folder. The log is saved as `mc3_360.log` in the same folder.

### Five files with names too long for the HDD

The 360's HDD and USB drives accept file names of up to 42 characters. Five loading screens in the game's `flash` folder have longer names, so the copy can fail or skip them. Rename them once, by hand, in the `flash` folder of your copy (on the PC, before copying, is easiest). The port finds them under the short name:

| Original name (in `flash`) | Rename to |
|---|---|
| `loadscreen_career_tournament_AtlDubOne.xbck` | `loadscreen_ca_tournament_AtlDubOne.xbck` |
| `loadscreen_career_tournament_AtlDubTwo.xbck` | `loadscreen_ca_tournament_AtlDubTwo.xbck` |
| `loadscreen_career_tournament_DetMuscle.xbck` | `loadscreen_ca_tournament_DetMuscle.xbck` |
| `loadscreen_career_tournament_SDCompound.xbck` | `loadscreen_ca_tournament_SDCompound.xbck` |
| `loadscreen_career_tokyo_TKSportsBikeClub.xbck` | `loadscreen_ca_tokyo_TKSportsBikeClub.xbck` |

Without them the game still works: only those loading screens don't show.

Back + Start held for 2 seconds returns to the dashboard.

## Option files

Files in the game folder, next to `default.xex`:

| File | Effect |
|---|---|
| `trafego.txt` | Two numbers from 0.2 to 1.0: traffic and pedestrian density. `1.0 1.0` is the original game, `0.7 0.7` is the default, lower numbers mean emptier streets |
| `lod.txt` | One number from 0.4 to 1.0: how far away models switch to their detailed versions and city props are drawn. `1.0` is the original, `0.75` is the default |
| `aspect43.txt` | Empty file: tells the game the TV is 4:3 |
| `sem_widescreen.txt` | Empty file: 720p in 4:3 |
| `sem_720p.txt` | Empty file: 640x480 output |
| `semintro.txt` | Empty file: skips the intro videos at boot |
| `nogamma.txt` | Empty file: keeps the TV's output gamma as it is |
| `debug_buttons.txt` | Empty file: L3 saves a debug capture, Back changes gamma |

## Credits

The runtime comes from my [Jet Set Radio Future](https://github.com/EduDicaseGameplay/JetSetRadioFuture-X360) and [Bloody Roar Extreme](https://github.com/EduDicaseGameplay/BloodyRoarExtreme-X360) ports for the Xbox 360, which use the [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool and [BloodyRoarExtremeRecomp](https://github.com/bromurok/BloodyRoarExtremeRecomp). Many fixes follow the Windows port [MidnightClub3-Port](https://github.com/christiand0797/MidnightClub3-Port): the fixed step clock, replays, career events, the depth copy for the headlights, level of detail and traffic density. The WMA music fix follows mc3port. Details and licenses are in `THIRD_PARTY.txt` and `pc-recomp`.

Midnight Club 3: DUB Edition Remix belongs to Rockstar Games and Take-Two Interactive. This is a fan project with no connection to them.
