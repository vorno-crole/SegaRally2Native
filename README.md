<p align="center">
  <img src="docs/img/marquee.png" alt="Sega Rally 2" width="100%">
</p>

<p align="center">
  <b>Sega's 1998 arcade rally game, running natively on PC.</b><br>
  Not an emulator: the original code, recompiled to C++, with a modern Vulkan renderer.
</p>

<p align="center">
  <a href="https://www.youtube.com/@snicolast"><img src="https://img.shields.io/badge/YouTube-@snicolast-red?logo=youtube&logoColor=white" alt="YouTube: snicolast"></a>
  <a href="https://buymeacoffee.com/snicolast8u"><img src="https://img.shields.io/badge/Buy_me_a_coffee-snicolast8u-FFDD00?logo=buymeacoffee&logoColor=black" alt="Buy me a coffee"></a>
</p>

<p align="center">
  <img src="docs/img/village.jpg" alt="Through a mountain village in true widescreen, with ray-traced sun shadows" width="100%">
</p>

> [!IMPORTANT]
> **This is an unofficial, modified fork that adds macOS support.** It is not made or endorsed by snicolast,
> the author of [Sega Rally 2 Native](https://github.com/snicolast/SegaRally2Native); all the credit for the port
> itself is theirs. The only thing this fork adds is a macOS build (branch `macos-intel`), kept on top of the
> upstream releases. For Windows, use the [original project](https://github.com/snicolast/SegaRally2Native) and
> its NicoLauncher. Please report macOS problems here, not upstream.

## macOS (this fork)

The same game, built and run natively on a Mac: the recompiled code compiled for the Mac's CPU, and the Vulkan
renderer running on Metal through [MoltenVK](https://github.com/KhronosGroup/MoltenVK). Windows builds of this fork
behave exactly as upstream's: every macOS change sits behind `__APPLE__`, with the Mac's code in `src/platform/`.

**Status:** playable on an Intel Mac with an AMD graphics card (tested on macOS 15 with a Radeon RX 6600 XT).
Apple silicon Macs are planned and not tested yet.

**Not on macOS:** ray tracing and DLSS/DLAA (MoltenVK and Apple GPUs have neither; the game uses its
non-ray-traced effects and MSAA, as on any GPU without them), the splash video (the splash picture shows
instead), UPDATE NOW (update with `git pull` and build again) and the other recomps in MORE RECOMPS.

**You need:** your own `srally2.zip` (the version described under "ROM not included" below),
an internet connection for the first build, and about 1 GB of disk. The same licence applies: nothing from the
ROM is in this repository, and nobody may share a compiled build, a `.app` or anything made from the ROM. You
build it yourself.

### Building on a Mac

1. **Tools**, once: Xcode's command-line tools and [Homebrew](https://brew.sh), then the libraries and compilers.

   ```sh
   xcode-select --install
   brew install llvm cmake ninja python molten-vk vulkan-loader
   ```

2. **The source**, from this fork's `macos-intel` branch:

   ```sh
   git clone -b macos-intel https://github.com/vorno-crole/SegaRally2Native.git
   cd SegaRally2Native
   ```

3. **Your ROM**, read into `data/` and its program recompiled into `build/gen/` (a few seconds; both folders stay
   out of git). Use Homebrew's Python: the one macOS has (`/usr/bin/python3`, 3.9) is too old for the tools.

   ```sh
   PY="$(brew --prefix)/bin/python3"
   "$PY" tools/romtool.py /path/to/srally2.zip data
   "$PY" tools/recomp.py data/crom.bin build/gen
   ```

   `romtool.py` names any chip that is missing or wrong in the zip.

4. **Build.** The first configure downloads SDL3 and miniz and takes 2 to 3 minutes; the build itself takes
   about a minute. The game is linked into the project folder, with MoltenVK and the Vulkan loader copied next
   to it, so it needs no Vulkan SDK or environment variables.

   ```sh
   LLVM="$(brew --prefix llvm)"
   cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_IN_SOURCE_DIR=ON -DPython3_EXECUTABLE="$PY" \
     -DCMAKE_C_COMPILER="$LLVM/bin/clang" -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" -DCMAKE_OBJCXX_COMPILER="$LLVM/bin/clang++"
   cmake --build build/mac
   ctest --test-dir build/mac   # optional: the unit tests, all should pass
   ```

5. **Play**, from the project folder (the game reads `data/`, `res/` and `textures/` there and keeps its
   settings and records in `save/`):

   ```sh
   ./SegaRally2
   ```

Keys and gamepads work as listed under [Controls](#controls); on a Mac, Option+Return toggles full screen and
Cmd+Q quits. Run it from a terminal to see its log there, which is what to include when reporting a problem.

**Updating:** `git pull`, then `cmake --build build/mac`. Run step 3 again only if `tools/` changed.

How the macOS code is laid out, what was measured and what is still open: [docs/MACOS.md](docs/MACOS.md).

## What you get

- **True widescreen**, not stretched, with the HUD at the corners.
- **Ray tracing**: shadows, global illumination and reflections on the cars.
- **Real dust**: billowing clouds and flying stones instead of flat sprites.
- **HD liveries and HUD**, redrawn for this port.
- **Sharp and smooth**: NVIDIA DLAA and DLSS, or MSAA, and 120 / 144 Hz and above.
- **Arcade sound**, with surround and headphone modes.
- **Keyboard, gamepad or wheel** (wheel experimental).
- **Achievements and statistics**: 14 achievements, your best laps and times per course, in RECORDS.

<table>
  <tr>
    <td><img src="docs/img/forest.jpg" alt="The forest road of the mountain stage"></td>
    <td><img src="docs/img/snow.jpg" alt="Snow thrown up by the wheels on the snowy stage"></td>
  </tr>
</table>

<p align="center">
  <img src="docs/img/menu.jpg" alt="Main menu" width="100%">
</p>

## ⚠️ ROM not included

You need your own **`srally2.zip`** (Sega Model 3 Step 2.0, files like `epr-20632.17`, with
`epr-20512.bin`, the force feedback board). Nothing from it is in this repository, and other
versions will not work. `build.bat` says which chips are missing.

## Install

You need Windows 10/11 (64-bit) and a GPU with a Vulkan 1.3 driver (most from 2016 on). Ray
tracing needs an NVIDIA RTX or an AMD RX 6000 or newer; DLAA and DLSS, an NVIDIA RTX. On other
cards those options switch themselves off, and the game builds the same on any GPU.

Install it with **[NicoLauncher](https://github.com/snicolast/NicoLauncher-Hub)**, a free app that
downloads, builds, updates and starts the game for you. No Visual Studio, nothing to set up by hand.

1. Download **[NicoLauncher-setup.exe](https://github.com/snicolast/NicoLauncher-Hub/releases/latest)** and run it.
2. In the launcher, open **Sega Rally 2**, press **Install** and choose your `srally2.zip` when it asks.
3. Press **Play**.

### Update

With NicoLauncher: it tells you when there is something new. Press **Update**.

Without it: when there is a new version, the main menu says what is new and offers **UPDATE NOW**. Or run
`update.bat` yourself: it downloads the changed files from GitHub (no git needed) and rebuilds.
Your ROM, settings and textures are kept; files you edited are backed up to `out\update_backup\`.

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Steer | Left / Right | Left stick |
| Accelerator / brake | Up / Down | RT / LT (or A / B) |
| Shift up / down | A / Z | RB / LB |
| VIEW CHANGE | V | Y |
| Hand brake | Space | X |
| Coin + start | 5, then 1 | BACK, then START |
| Pause | Esc | START |

F11 or Alt+Enter toggles fullscreen. Everything else is in **Options**. In the selection screens
"confirm with the pedal" means the accelerator. The gamepad rumbles with the wheel's force feedback.

**Steering wheels (experimental)** are read through SDL, with force feedback. This has never been
tested on a real wheel: the settings are in `save\wheel.ini`, and `set SR2_WHEEL_LOG=1` prints
every axis and button you move. If you have one, please tell me how it goes in an issue.

## Performance

OPTIONS > DISPLAY > **QUALITY** sets the costly effects together, from LOW (laptops) to ULTRA.
On an RTX, **ANTI-ALIASING: DLSS QUALITY** draws the 3D at two thirds of the size and rebuilds it
sharp, about 30% lighter on the GPU than DLAA: the one to try first at 1440p, 4K or 120 Hz and above.

## More

- `SegaRally2.exe --help` lists the command-line options (`--aspect 21:9`, `--hud-centre`...).
- Textures of your own: `--dump-textures` writes the game's to `out\textures\`; a redrawn one
  in `textures\` with the same name replaces it.
- [How it works](docs/ARCHITECTURE.md): the PowerPC to C++ recompiler and the hardware.
- Other recompiled arcade games: [The Lost World](https://github.com/snicolast/LostWorldNative)
  and [Star Wars Trilogy](https://github.com/snicolast/StarWarsTrilogyNative).

## License

The code in this repository is under the [MIT License, with additional terms](LICENSE). The libraries and fonts in
`third_party/` keep their own licenses. The gamepad button pictures in `res/pad/` are
[Controller Input Icons](https://opengameart.org/content/controller-input-icons) by ElDuderino (CC0).
The game itself (ROM, code, data, music and voices) is not included and stays the property of its
owners: you need your own copy of `srally2.zip`.

**No prebuilt downloads.** The license does not allow anyone to share a compiled build of this
port, a ROM, or anything made from one. I have never published any of them: everyone builds the
game themselves from their own `srally2.zip`. Any ready-to-play download you find elsewhere is
not from me, breaks the license, and is at your own risk.

<sub>A fan project for personal use, not affiliated with or endorsed by Sega or by any of the car
makers whose cars appear in the game. Sega Rally is a trademark of Sega; the car names and
liveries belong to their owners.</sub>
