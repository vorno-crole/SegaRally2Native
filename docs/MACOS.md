# Sega Rally 2 Native on an Intel Mac

Status: **it boots and draws on macOS: the SEGA logo, then the 3D attract demo with the HD liveries and text, through MoltenVK with no errors.** Menus, controls and a race not tried yet. Branch
`macos-intel`, rebased onto upstream `main` at `d607393` (the project has no release tags). An Apple silicon port
follows this one, so the macOS code is kept free of x86-only code.

## Building on macOS

```sh
brew install llvm cmake ninja python molten-vk vulkan-loader
python3 tools/romtool.py <path to>/srally2.zip data
python3 tools/recomp.py data/crom.bin build/gen
LLVM=$(brew --prefix llvm)
cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME_IN_SOURCE_DIR=ON \
  -DCMAKE_C_COMPILER=$LLVM/bin/clang -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ -DCMAKE_OBJCXX_COMPILER=$LLVM/bin/clang++
cmake --build build/mac
ctest --test-dir build/mac
./SegaRally2
```

The game is linked into the project folder (it reads `data/`, `res/`, `textures/` and `save/` from the folder it is
started in), with `libMoltenVK.dylib`, `libvulkan.1.dylib` and `MoltenVK_icd.json` copied next to it. It points the
Vulkan loader at that json itself, so no environment variable or Vulkan SDK is needed.

## How the macOS code is laid out

`src/platform/`, every file `mac_*`:

| File | What it does |
|---|---|
| `mac_win32.h` | the part of `<windows.h>` the game uses, included in its place under `__APPLE__`, so most shared files change only at their include |
| `mac_win32.mm` | files, clocks (`mach_absolute_time`), threads (pthreads, with the game thread's 512 MB stack), alerts, UTF-8, links in the browser, the achievement chime (`NSSound`) |
| `mac_window.h/.mm` | the AppKit window with a `CAMetalLayer`; it sends the presenter's window procedure the same Win32 messages (key codes, mouse in window pixels, `WM_SIZE`, `WM_CLOSE`) and keeps the key state |
| `mac_gdi.mm` | the GDI text of the menus and the achievement notice on Core Text: fonts from memory matched by their GDI names, OS/2 win metrics, no kerning or ligatures, grey antialiasing |
| `mac_audio_out.mm` | `AudioOut` on Core Audio's default output unit (replaces `audio/audio_out.cpp`) |
| `mac_image_io.mm` | PNG load and save through ImageIO, straight alpha, no colour conversion (replaces `video/image_io.cpp`); also the menus' pictures |
| `mac_net.mm` | the news download through `NSURLSession` |
| `mac_splash_video.mm` | not done yet: reports no video, so the menu shows the splash picture (replaces `ui/splash_video.cpp`) |

Shared files with `__APPLE__` branches: `vk_presenter.*` (window, MoltenVK loader, portability enumeration and
subset, Metal surface, present wait off, failing Vulkan calls logged), `r3d_renderer.cpp` (state bound again after
MoltenVK's clear), `frontend.cpp` (the news download and the image loader), `ppc_rt.h` and `r3d_scene.cpp`
(compiler builtins for the MSVC intrinsics), `CMakeLists.txt`. Elsewhere only the `<windows.h>` include changes.

On macOS: UPDATE NOW opens the GitHub page (there is no `update.bat`), the other recomps in MORE RECOMPS are never
found (they are Windows programs), and the display timing uses the game's own clock, which measured smoother than a display link.

macOS debugging switches: `SR2_MAC_VBLANK=1` (display-link vblank times), `SR2_MAC_PRESENT_WAIT=1`,
`SR2_MAC_NO_FOCUS=1` (window in sight without the keyboard), plus `MTL_SHADER_VALIDATION=1 MTL_DEBUG_LAYER=1`
for Metal's validation.

The aim is a macOS build of the same game, on an Intel Mac with an AMD GPU through MoltenVK. Windows behaviour stays
as it is. Every macOS change sits behind `__APPLE__` (or `if(APPLE)` in CMake), beside the existing `_WIN32` code.
The licence terms apply here as everywhere: you build it yourself from your own `srally2.zip`. No compiled build,
`.app` or generated code is ever shared.

## Target machine

| | |
|---|---|
| Machine | Hackintosh, macOS 15.4 (24E248) |
| CPU | Intel Core i7-9700K (x86-64; AVX2, BMI1/2, FMA, MOVBE: `x86-64-v3` is safe) |
| GPU | AMD Radeon RX 6600 XT, 8 GB (RDNA 2) |
| RAM | 32 GiB |
| Displays | 2560x1440 (main), 2560x1440, 1920x1080 |
| Vulkan | MoltenVK 1.4.2 (Homebrew), reports Vulkan 1.4.357 |
| Toolchain found | Homebrew under `/usr/local`: LLVM/clang and lld 23.1.4, CMake 4.4.4, Ninja 1.13.2, Python 3.15.1, vulkan-loader/headers/tools 1.4.363, SDL3 3.4.18 |

## What the project is, and what that means for a Mac

The arcade game's PowerPC 603e code is statically recompiled by `tools/recomp.py` into C++ (`build/gen/`, made from
the ROM, never committed), linked with a hand-written runtime that emulates the Model 3 hardware (Real3D GPU through
Vulkan, SCSP sound through Musashi, the Z80 drive board).

**CPU side: fits.** The generated code is plain C++ that compiles to x86-64. Guest RAM is an 8 MB `calloc` reached
through inline `rd*`/`wr*` helpers. There is no fixed-address reservation, no fault handling, no write watching and
no host memory import, so the memory measurements an emulator port needs do not apply here. Page size (4 KiB) is the
same as on Windows. What needs care:

- `ppc_rt.h` uses MSVC intrinsics (`_byteswap_ulong`, `_byteswap_ushort`, `_BitScanReverse`) and `r3d_scene.cpp` uses
  `__popcnt`: map them to `__builtin_bswap32/16`, `__builtin_clz` and `__builtin_popcount`.
- The game's floating point must not change. The clang-for-Windows build passes `-ffp-contract=off
  -fno-strict-aliasing -fwrapv`, but only under `elseif(WIN32)`; the macOS build needs the same flags.
- macOS is LP64: `long` is 64-bit (32-bit on Windows). The few uses (`drive_board.cpp`, `menu_music.cpp`) look safe,
  but are worth reading again. `wchar_t` is 32-bit, which matters for the UI's wide strings.
- The game thread is created with a 512 MB stack reservation (deep nesting of the recompiled calls): on macOS that
  is a `pthread_attr_setstacksize` of the same size.

**Platform side: all Windows.** There is no Linux port to follow. The code calls Win32 and COM directly in a dozen
files, with five `#ifdef _WIN32` guards in the whole tree. Every platform service has to be written for macOS:

| Service | Windows today | Where | macOS plan |
|---|---|---|---|
| Window, messages, cursor, fullscreen | `CreateWindowExA`, `WndProc`, `PeekMessage`, `SetWindowPos`, monitor info | `vk_presenter.*` | AppKit window on the main thread, `CAMetalLayer`, `VK_EXT_metal_surface` |
| Vulkan surface | `VK_KHR_win32_surface` | `vk_presenter.cpp` | `VK_EXT_metal_surface` plus portability enumeration and subset |
| Keyboard | `GetAsyncKeyState`, `GetKeyState`, `GetForegroundWindow` | `app.cpp`, `app_input.cpp`, `frontend.cpp` | key-state table filled from the window's key events |
| Gamepads, wheels | SDL3 (joystick/gamepad/haptic only) | `gamepad.cpp`, `wheel.cpp` | unchanged: SDL3 works on macOS (IOKit, GameController) |
| Sound out | WASAPI shared mode, own thread | `audio_out.cpp` | Core Audio output unit (or SDL3 audio) behind the same `AudioOut` |
| Achievement sound | `PlaySoundW` | `achievement_toast.cpp` | play through the game's own output, or `NSSound` |
| Menu and toast text | GDI: `AddFontMemResourceEx`, `CreateFontW`, `TextOutW` into a DIB section | `frontend.cpp`, `achievement_toast.cpp` | Core Text into a `CGBitmapContext`, same bundled fonts |
| PNG read and write | Windows Imaging Component | `image_io.cpp`, `frontend.cpp` | ImageIO framework |
| Splash video | Media Foundation `MFMediaEngine` | `splash_video.cpp` | AVFoundation; skipped until then (it already falls back when the file is missing) |
| News and update check | WinHTTP | `frontend.cpp` | `NSURLSession` |
| Open a link | `ShellExecuteW` | `frontend.cpp` | `NSWorkspace openURL` |
| UPDATE NOW | runs `update.bat` | `frontend.cpp` | hidden on macOS (or a message to `git pull` and rebuild) |
| Display timing | `DwmGetCompositionTimingInfo`, `timeBeginPeriod` | `app.cpp` | `CVDisplayLink` / `NSScreen` refresh rate; no timer resolution call needed |
| Clock | `QueryPerformanceCounter`, `GetTickCount64` | `app.cpp`, `runtime.cpp`, `frontend.cpp` | `mach_absolute_time` or `std::chrono::steady_clock` |
| Files and folders | `CreateDirectoryA`, `GetFileAttributes*`, `FindFirstFileA`, `GetFullPathName*`, `GetModuleFileName*`, `MoveFileExA` | `app*.cpp`, `runtime.cpp`, `stats.cpp`, `texture_pack.cpp`, `sky_pack.cpp`, `image_io.cpp`, `frontend.cpp` | POSIX (`mkdir`, `stat`, `opendir`, `realpath`, `_NSGetExecutablePath`, `rename`) |
| Threads | `CreateThread` with a 512 MB stack; elsewhere `std::thread` | `app.cpp` | `pthread` with the same stack size |
| Messages to the user | `MessageBoxA`, `AttachConsole` | `app.cpp`, `app_files.cpp` | `NSAlert`; stdout already goes to the terminal |
| CRT | `localtime_s`, `_putenv`, `_wcsicmp` | `runtime.cpp`, `app.cpp`, `frontend.cpp` | `localtime_r`, `setenv`, `wcscasecmp` |
| Icon | `res/app.rc` | CMake | not built on macOS; an `.icns` only if a launcher `.app` is made |
| DLSS / DLAA | NVIDIA NGX | `dlss*.cpp` | `dlss_stub.cpp`, as on any GPU without it (MSAA instead) |

The service seams are already narrow (`AudioOut`, `png_load`/`png_save`/`list_files`, `VkPresenter`, the UI's text
rasteriser), so the macOS code can sit in its own files behind them.

## GPU requirements against MoltenVK 1.4.2 on the RX 6600 XT

Read with `vkcaps.sh` and `vulkaninfo --show-formats`. "Fallback" means the renderer already checks for it and runs
without it.

| Need | Required? | On this GPU | Verdict |
|---|---|---|---|
| Vulkan 1.3 device (`apiVersion` check in `vk_presenter.cpp`) | yes | 1.4.357 | works |
| `dynamicRendering` | yes | true | works |
| `descriptorBindingPartiallyBound`, `descriptorBindingUpdateUnusedWhilePending`, `shaderSampledImageArrayNonUniformIndexing` | yes | true | works |
| `runtimeDescriptorArray`, `descriptorBindingSampledImageUpdateAfterBind` (upstream's unmerged `fix-intel-texture-limit` uses them) | with that fix | true | works |
| `samplerAnisotropy` (16x) | yes | true, max 16 | works |
| `independentBlend` | DLSS only | true | works (DLSS is off on macOS anyway) |
| Swapchain `B8G8R8A8_UNORM`, FIFO | yes | yes | works |
| `VK_KHR_present_id` + `VK_KHR_present_wait` | optional (frame pacing) | true | works |
| Ray queries (`VK_KHR_ray_query`, acceleration structures) | optional | **absent** | fallback: shadow maps and the non-RT shaders, as on a GPU without RT. No traced shadows, GI or reflections on macOS |
| DLSS / DLAA | optional | NVIDIA only | fallback: stub, MSAA |
| MSAA | optional | 1, 2, 4 samples (no 8) | fallback: `r3d_renderer.cpp` clamps to the supported counts |
| Formats: `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `R16G16B16A16_SFLOAT`, `R16G16_SFLOAT`, `R16_SFLOAT` (sample, filter, attachment, blend) | yes | all supported | works |
| `R16_UINT` texture RAM (2048x2048, `texelFetch`), `R32_UINT` | yes | sampled, no filtering needed | works |
| `D32_SFLOAT` depth, sampled (shadow map, soft particles) | yes | supported | works |
| Vertex input: 72-byte stride, every attribute 4-byte aligned, `R32G32B32_SFLOAT` etc. | yes | `minVertexInputBindingStrideAlignment` 4 | works |
| Timestamps (GPU timing) | debug | true | works |
| **Texture pack array: 4096 combined image samplers** in one binding (`r3d.frag`, `r3d.vert`) | yes | `maxPerStageDescriptorSamplers` 16; update-after-bind samplers 2048 per stage and per set | works: upstream sizes the array from the GPU's limits with update-after-bind since `4bc9960`, 2032 slots here (the full 4096 also worked on MoltenVK before that) |
| **Compose pass: 64 + 12 samplers in the fragment stage** | yes | 16 per stage with direct binding; more only through Metal argument buffers | **visible risk**: MoltenVK uses argument buffers on macOS 15, so it should work, but check `[mvk-error]` and the 2D layers early |
| Geometry shaders, `shaderFloat64`, robustness2, custom border colours, LOD bias, 3D images, triangle fans, 16-bit packed formats, push descriptors | — | missing or emulated | not used by this renderer |
| Subgroup ops | — | no vertex-stage subgroups | not used |

## Risks, most likely to matter first

1. **The platform layer is written from scratch.** There is no Linux port: window, keyboard, sound, text,
   images, HTTP and video are all Win32 or COM, about twenty files to touch. Most of the effort of phases 2 and 3 is
   here. Keeping the shared files' changes to small `__APPLE__` branches keeps rebases cheap.
2. **Descriptor limits for the texture pack.** On `main`, the 4096-entry sampler array is beyond what MoltenVK
   allows, so the 3D renderer's set layout or pipelines may fail. Upstream's `fix-intel-texture-limit` solves the
   same problem for Intel GPUs and gives 2032 slots on this card. Missing replacement textures (HD liveries) would be
   the symptom of getting this only half right.
3. **Menu and HUD text through Core Text instead of GDI.** Font metrics, variable-font weights (`Exo2[wght]`,
   `Arimo[wght]`, `Orbitron[wght]`) and wide-string handling (`wchar_t` is 32-bit) all differ, so text can be offset,
   clipped or the wrong weight. Compare screenshots with Windows ones, screen by screen.
4. **Floating-point flags.** Without `-ffp-contract=off -fno-strict-aliasing -fwrapv` clang may fuse multiply-adds
   and the car physics drift from the arcade's. Cheap to get right, silent if missed.
5. **Compose pass beyond 16 samplers** (see the table). Expected to work through argument buffers; check early.
6. **Frame pacing.** The game runs at the arcade's 57.524 Hz and interpolates to the display. The pacing reads DWM's
   timing; on macOS it needs the display link and present wait. Keep the game on a 60 Hz display at first.
7. **No ray tracing on macOS.** MoltenVK has no ray queries. The fallback exists and is the same as on a GPU
   without RT, but the Mac will never show the traced shadows, GI or reflections. Not a bug to chase.
8. **First-run pipeline stalls.** MoltenVK has no graphics pipeline library; Metal compiles each pipeline on first
   use. The renderer has few pipelines, so this should be short.

## Phases

| Phase | Done when | Rough effort |
|---|---|---|
| 0. Assessment | requirements checked against this GPU (this document). No memory or fault costs to measure: the runtime has none of those mechanisms | done |
| 1. Toolchain | CMake configures with Homebrew clang on macOS; shaders, Musashi tables and the nine unit tests build and pass; MoltenVK 1.4.2 and its ICD json sit beside the executable; `.gitignore` covers the macOS outputs | done: 10 of 10 tests pass, 11 shaders |
| 2. Runtime builds | the game links from `build/gen` (needs the ROM), the platform services have macOS versions or loud stubs, and `--headless` runs the game code with sound | done: `--headless` runs 600 frames in 8 s (about 150 fps uncapped) and ends cleanly |
| 3. Boot | window, Vulkan device, title screen and menus with their text | window, device and attract demo done; the main menu not seen yet |
| 4. Correct | the title, menus and first stage match Windows screenshots of the same scenes | open |
| 5. Play | a full race at full frame rate, settings and records written and read back | open |
| 6. Package | a launcher `.app` that runs from Finder (for your own use only: the licence forbids sharing builds) | half a day |

### Phase 1 in detail

- Add to `.gitignore` before anything is generated: the macOS executable (`SegaRally2` without `.exe`),
  `*.dylib`, the MoltenVK ICD json and any `*.app`. The rest (`data/`, `build/`, `out/`, `save/`, `*.zip`,
  `third_party/downloads/`) is already covered.
- CMake, under `if(APPLE)`: no `res/app.rc`, no Windows libraries, `VK_USE_PLATFORM_METAL_EXT`, the
  floating-point flags, the Apple frameworks, `-mmacosx-version-min`, and Objective-C++ for the macOS files. Keep
  `GAME_IN_SOURCE_DIR` working.
- Build `shaders`, `m68kmake` and the tests first: they use no Windows API.

### Phase 2 in detail

- `ppc_rt.h` and `r3d_scene.cpp` intrinsics, `localtime_r`, `setenv`, paths and folders, the 512 MB game thread.
- macOS files behind the existing seams: `audio_out` (Core Audio), `image_io` (ImageIO), file listing, clock.
- Stubs that log and return for the UI pieces still to do (text, video, HTTP), so the game links.
- Generate `build/gen` from your ROM with `romtool.py` and `recomp.py`, link, then run `--headless` for a timed
  run with sound.

### Phase 3 in detail

- `main` keeps the window and Vulkan on the main thread (it already runs the game on its own thread).
- `VkPresenter` on macOS: an AppKit window with a `CAMetalLayer`, the portability flag and extensions, events for
  keyboard, resize, fullscreen and close.
- Merge or cherry-pick upstream's texture-limit fix, run with `MVK_CONFIG_LOG_LEVEL=2`, and read every
  `[mvk-error]`.
- Core Text rasteriser for the menus and the achievement toast.

## Measurements

| Date | Test | Result |
|---|---|---|
| 2026-11-13 | unit tests (`ctest`) | 10 of 10 pass |
| 2026-11-13 | `--headless --volume 0` | boots, reaches the attract sequence; 600 frames in 8 s, ~150 fps uncapped; exit 0 |
| 2026-11-13 | window, `--shot 600` | RX 6600 XT through MoltenVK 1.4.2; MSAA 4x, anisotropic 8x; 59 2D replacements, 252 HD text patterns; SEGA logo and the credit (Core Text) drawn; no `[mvk-error]` |
| 2026-11-13 | window, `--shot 1700` | 3D attract demo: ~11,900 polygons, HD liveries from the texture pack (the 4096-slot array works on MoltenVK), dust, HUD text. Per frame: game 6.3 ms, scene 9 ms, GPU wait 2-3 ms |

2026-11-13, first play by the user: after PLAY's unseen run to SELECT GAME, presents fell to exactly 20 a second
(53 ms apart) for good: `VK_KHR_present_wait` waits on MoltenVK timed out (app.cpp waits up to 50 ms a frame),
and even before that presents wandered between 70 and 95 a second. Present wait is now off on macOS
(`vk_presenter.cpp`); pacing is vsync alone (to measure again: the 20 a second turned out to be the GPU hang below).

**GPU hang on the screens after SELECT GAME (fixed 2026-11-13).** The picture froze while the game ran on: a Metal
command buffer timed out (`kIOAccelCommandBufferCallbackErrorTimeout`) and the device was lost. Not the texture
pack, the HD 2D pictures or the anisotropic filter (each switched off in turn, same hang at the same frame).
`MTL_SHADER_VALIDATION=1 MTL_DEBUG_LAYER=1` named it: a 3D draw read its 65,600-byte descriptor set from a
160-byte buffer. Screens with two 3D priorities clear the depth between them with `vkCmdClearAttachments`;
MoltenVK draws that clear with its own fragment buffer 0 (its clear colours, 160 bytes) and does not bind the
app's descriptor set back. The attract demo has one priority, so it never showed. Fix: `r3d_renderer.cpp` binds
the 3D set and the vertex buffer again after that clear, on macOS. Verified under Metal validation.

2026-11-13, rebase onto `d607393`: upstream's first-run anti-aliasing reads the screen's mode (`screen_height`, on
macOS from Core Graphics) and its terminal-prompt fix is Windows-only (a macOS terminal waits for the game).
Headless identical to before (600 frames, same cycle count); windowed under Metal validation through the select
screens: no errors, texture pack limited to 2032 images by the GPU.

2026-11-13, frame pacing, measured with `LW_PACING=1` on the attract demo, window visible on the 60 Hz main
screen, six interleaved runs of 40 s (A B C A B C). Dispersion of the motion per present, each figure 5 s
(lower is more even; speed was 1.000 in all):

| Version | Run 1 | Run 2 | Frozen steps per 5 s |
|---|---|---|---|
| A: app.cpp's clock that follows the presents (no vblank time) | 0.010, 0.014, 0.016 | 0.007, 0.011, 0.014 | 0 |
| B: vblank time from a Core Video display link | 0.288, 0.061, 0.128 | 0.120, 0.028, 0.018 | 1 to 6 |
| C: B with present wait | 0.012, 0.101, 0.017 | 0.006, 0.116, 0.004 | 0 to 2 |

A is the default. B (`SR2_MAC_VBLANK=1`) and present wait (`SR2_MAC_PRESENT_WAIT=1`) stay as switches for later
tries. All ran at 59 to 60 presents a second. The odd display rates of the earlier runs (645 Hz, 92 to 136 Hz)
came from the window being kept behind others (`LW_BACKGROUND`): timing tests need it in sight
(`SR2_MAC_NO_FOCUS=1` shows it without taking the keyboard). Keep the game on a 60 Hz screen: the second
2560x1440 here runs at 74.88 Hz.

To compare with Windows: the dust looks like flat sheets; a short black line at the top left of the SEGA logo
screen; the display rate the pacing log guesses swings (645 to 68 Hz) because there is no vblank time on macOS yet.
