# AnyPS5: Astro Bot (PPSA21564) fork

This fork is where I work on running Astro Bot (PPSA21564) on Linux with AnyPS5.

## Where the work is now (12 October 2026)

- **`astrobot` is frozen as a reference build.** It is upstream `main` from early October plus fixes that were specific to this setup or still on their way upstream. This fork's `main` mirrors it. The "reference build" column below describes it.
- **Development has moved to upstream `main`.** The aim is for the game to run there with everything going in as ordinary pull requests, one topic at a time.
- **On upstream `main` today** the game reaches the title screen, the NEW GAME menu, the first level and Space Station with four open pull requests applied (#425, #454, #2490, #2491). It runs at roughly half the reference build's frame rate there; the last measurements are in the "upstream `main`" column below.
- **What is left on `main`:** most of the gap is the queue worker doing all the per-draw preparation on one thread, where the reference build does it ahead on a second one. Next are #290 (host import windows) with #4221, and the depth surface work in #425 and #454.

Pull requests to upstream are always cut from upstream `main`.

## Astro Bot status

Linux, measured on my machine (CachyOS, i9-14900KF, RTX 4090). Frame rates are the game's own, with no frame generation. The reference build is `astrobot`, measured on 8 October 2026. The upstream `main` column is `main` of 11 October with #425, #454, #2490, #2491 and the other open pull requests named above, measured on 12 October while the machine was also running builds and tests, so those numbers are on the low side.

| Part | State | Reference build (`astrobot`) | Upstream `main` (12 October) |
| --- | --- | --- | --- |
| Boot, PlayStation Studios video, logos | Renders | video at about 57 fps | not measured |
| Title screen | Renders, including the copyright line (console fonts or the Noto substitutes) | about 60 fps | about 25 fps |
| NEW GAME menu | Renders | about 60 fps | about 23 fps |
| Intro cinematic and space scene | Renders | about 50 fps, robot crowd about 23 fps | about 28 fps, robot crowd about 13 fps |
| Tutorial (Crash Site hub) | Playable with a DualSense | about 16 fps on foot | not measured |
| World map, controller ship flight | Renders | about 32 fps | not measured |
| Sky Garden | Reached and flown | about 4.4 fps | not measured |
| Snowy Canyon | Reached | about 5.5 fps | not measured |
| Space Station | Reached (ship fly-in) | about 7.4 fps | about 3 fps |
| Dirty Bath | Reached, hot spring water renders | about 4.2 fps | not measured |
| Other levels | 24 levels booted with `-lvl`: all load, no GPU hangs | 3 to 53 fps (see below) | not measured |
| Windows | In game up to the first Gorilla Nebula level ([report](https://github.com/boykopovar/AnyPS5/discussions/357)) | | |

Level sweep (7 October, 150 s per level from a `-lvl` start, mostly the arrival fly-in and first platform): main levels run at about 3 to 14 fps (Pirate Island about 3, Clock Tower 4, Cymbal Party 4.6, Sky Aztec and Construction 5, Gulliver 8, Penguin Atlantis 8.7, Casino 9.3, Day and Night 14), bosses, heroes and minis at 10 to 40 fps, the hub dioramas at 20 to 53 fps. The two levels that hung the GPU on 4 October (Astro skin diorama, Cymbal Party) run clean now.

Known issues: the frame rate in levels (the queue worker's CPU time per draw is the main limit; indirect draws and occlusion box draws are the biggest items in Pirate Island and Space Station), shader compile stalls of several seconds up to about a minute on the first visit to a level after a shader change, thin crossing shards at the end of the Frozen Meal (ice_iceberg) fly-in, a short white flash on the world map, and occasional minor glitches.

New on this branch (7 and 8 October):
- **NGG passthrough fix:** passthrough geometry now takes the vertex path, with its allocation, export mask and wave fields proved exactly. The same code runs Dreaming Sarah clean at about 60 fps and Astro Bot. Those draws now hit the draw templates: the hub on foot is about 16% faster, the world map about 10%, the final boss arena went from about 25 to 40 fps.
- **Vertex reuse in NGG draws:** the robot crowd in the intro went from about 11 to 23 fps.
- **Texture host copies:** under host memory pressure the host copy of a sampled texture is released instead of the texture itself. Space Station frame time went from about 286 to 220 ms (about 3.5 to 4.5 fps, +30%), Pirate Island about 7% faster (`APS5_NO_TEXTURE_HOST_RELEASE=1` turns it off).
- **Texture cache size:** the sampled-texture cache may now use half of the device budget instead of 3/8, and cached textures are charged the bytes their image actually holds (accounting by ThisIsAkill). Cache evictions on Space Station dropped from about 2000 to a handful per 10 s; frame time went from about 224 to 135 ms (about 4.5 to 7.4 fps, +66%) and Pirate Island from about 306 to 257 ms (+19%). Peak VRAM use is about 2.5 GiB higher; on 8 and 12 GB cards the hard limit already applies, so nothing changes there.
- **CMASK fast clears:** fast clears on colour targets without DCC are now applied (a GPU pass in stream order). This fixes the stale lavender rectangles and the opaque tan water in Dirty Bath; the hot spring is translucent again (before and after below).
- **Level sweep:** 24 levels from all six galaxies, the hub dioramas, bosses, heroes and minis load with 0 crashes and 0 GPU hangs. The slowest are Pirate Island, Space Station and Dirty Bath at about 3 to 5 fps.
- **Next:** see "Where the work is now" above. This branch gets no further changes.

<img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/title.jpg" width="400" alt="Title screen"> <img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/menu.jpg" width="400" alt="NEW GAME menu">
<img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/intro-space.jpg" width="400" alt="Intro space scene"> <img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/crowd.jpg" width="400" alt="Robot crowd in the intro">
<img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/hub.jpg" width="400" alt="Crash Site hub"> <img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/map.jpg" width="400" alt="World map">
<img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/casino.jpg" width="400" alt="Slo-Mo Casino"> <img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/space-station.jpg" width="400" alt="Space Station">
<img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/bath-before.jpg" width="400" alt="Dirty Bath before the CMASK fix: opaque water with stale rectangles"> <img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/bath-after.jpg" width="400" alt="Dirty Bath after the CMASK fix">

Dirty Bath water before (left) and after (right) the CMASK fix.

## Level select (F2)

A level select for testing: it restarts the game straight into any level in the game's own list, without playing through to it or needing a save. Linux only for now.

```sh
APS5_LEVEL_MENU=1 ./app.elf
```

Press F2 in game to open it. It opens as a separate window and lists the 114 entries of the game's level list (`product_levels.xml`) grouped by category: hub, main levels by galaxy, secret levels, bosses, heroes, minis and so on, with the game's internal Meta entries last.

- Up/Down, PgUp/PgDn, Home/End or the mouse wheel move the selection. Typing filters the list (words match the category, name or level file, in any order); Backspace deletes, Ctrl+Backspace clears.
- Enter (or a double click) restarts into the selected level: the process re-executes itself with the same command line and environment plus the game's own `-lvl <level file>` argument. Pending save-data writes get up to 2 s to finish first.
- F2 or Esc closes the menu.

<img src="https://raw.githubusercontent.com/oneandonlydean/AnyPS5/3869aaca7562c8c5ac818544078edc083a9a3552/readme/2026-10-08/level-menu.png" width="340" alt="F2 level select filtered to G1">

To pick a level without touching the keyboard, for scripted runs:

```sh
APS5_LEVEL_MENU=1 APS5_LEVEL_MENU_AUTOSELECT=20:rocket_space_station ./app.elf
```

After 20 seconds this opens the menu, types `rocket_space_station` as the filter and presses Enter on the first match. The variable is dropped on the restart, so it fires once. Booting a level directly with `./app.elf -lvl rocket_space_station` does the same without the menu.

Caveats: sublevels such as `enemy_final` start standalone. `diorama_zoo` loads but the game reports an empty level name. The menu falls back to a built-in bitmap font when no DejaVu, Noto or Liberation font is installed (`APS5_LEVEL_MENU_FONT` names another TrueType file).

## Upstream contributions

Pull requests from this work merged into [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) include Linux write tracking (#120, #121), the shader disk and pipeline cache (#129), NGG geometry as mesh shaders (#133), resident 10-bit scanout (#278), the Linux argv fix behind `-lvl` (#418), flips that complete after the frame's GPU work (#417), large DMA copies on the GPU in queue order (#439), triangle fan geometry input (#440), occlusion counter dumps on the GPU (#463), cross-queue submission order (#465), exact reciprocals for `--to-intel` (#464) and DualSense output and audio (#179, #180). Recent: host-import spans joined only over whole windows (#3425), a device-loss fix for NVIDIA 610 drivers (#3472) and a render-extent fix for stencil tests without a stencil plane (#3702).

Open: #2490 and #2491 (depth plane copies), #3874 (import window compare), #4221 (in-place index ranges).

---

# About

Tool for automatic executables porting to Linux and Windows.

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Status

[![libraries](https://boykopovar.github.io/AnyPS5/badge-libraries.svg)](https://boykopovar.github.io/AnyPS5/) [![shaders](https://boykopovar.github.io/AnyPS5/badge-shaders.svg)](https://boykopovar.github.io/AnyPS5/)

[![progress map](https://boykopovar.github.io/AnyPS5/progress.svg)](https://boykopovar.github.io/AnyPS5/)

<sub>* System libraries: percentage of the functions known to the project so far (declared in [core/libs/prx](core/libs/prx)), not of every PS5 system function. The total grows as more functions are declared.</sub>

[List of verified games](docs/user/COMPATIBILITY.md)

Dreaming Sarah (2D platformer) runs at a stable 60 fps on a GTX 1050 Ti / i5-7500 3.4GHz.

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Compatibility

See the [game compatibility list](docs/user/COMPATIBILITY.md) for tested games and known issues.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## System fonts

Games that open the console's system font sets need font files in an `anyps5-fonts` directory beside the generated game executable; set `ANYPS5_SYSTEM_FONTS` to use another directory. Files dumped from the console are used under their own names (`SST-Roman.otf`, `SST-Bold.otf`, `SSTJpPro-Regular.otf`, ...). Without them, these openly licensed substitutes are used when present: `NotoSans-{Light,Regular,Medium,Bold}.ttf` and `NotoSans-{LightItalic,Italic,MediumItalic,BoldItalic}.ttf` (Latin and Vietnamese), `NotoSansMono-{Light,Regular,Medium,Bold}.ttf` (typewriter), `NotoSansThai-{Light,Regular,Medium,Bold}.ttf` (Thai) and `NotoSansCJK-{Light,Regular,Medium,Bold}.ttc` (Japanese and Chinese). Without either, opening a system font set fails and the game shows no text in those fonts.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
