# The capture viewer

`SegaRally2Viewer.exe` opens a snapshot of the game, one frame exactly as the
game sent it to the Real3D board, and lets you fly around it: a free camera,
wire frame, the faces the game's camera could not see, a click to pick a model
and a list of the textures that model uses.

It is a program of its own next to the game. It draws with the game's scene
builder and Vulkan renderer (`src/video`), unchanged, and links none of the
recompiled code: its own sources are in `src/viewer`.

## Building

`build_viewer.bat`, with the same tools as `build.bat` (Visual Studio 2022 with
the C++ tools; `install.bat` installs them). It uses the same build folder, so
after a build of the game only the viewer's own files are compiled. The viewer
is not part of the normal build: `build.bat` neither builds nor needs it.

By hand, in a build folder that is already configured:

    cmake --build build\cmake --target SegaRally2Viewer

No ROM is needed to build it. To show a snapshot it reads `data\vrom.bin` (the
models), which `build.bat` extracts from the ROM.

## Using it

1. In the game, press **F9** at the moment you want to look at. The frame is
   saved to `out\snap\snap_<frame>.bin`.
2. Run `SegaRally2Viewer.exe`. It opens the newest snapshot in `out\snap`; a
   snapshot can also be dragged onto it or given as an argument.

```
SegaRally2Viewer [snapshot.bin] [--data DIR] [--textures DIR] [--msaa N]

  snapshot.bin    a snapshot saved by the game with F9. Without it: the newest
                  one in out\snap, or a file dialog
  --data DIR      the folder with vrom.bin (default: data, here or next to the program)
  --textures DIR  the texture pack to look replacements up in (default: textures)
  --msaa N        samples per pixel: 1, 2, 4 or 8 (default 4)
```

Snapshots hold data of the ROM (models, textures): like `data\` and `out\`,
they are for your own use and are not to be shared.

## Keys

| | |
|---|---|
| Alt + left button, dragged | orbit about what is in front of the camera |
| Alt + middle button, dragged | slide the view sideways and up or down |
| Alt + right button, dragged | nearer (to the right or down) or farther |
| Mouse wheel | forward, back |
| Right button, held | look around |
| W A S D | fly |
| Q, E | down, up |
| Shift, Ctrl | fly faster (x4), slower (x0.25) |
| Mouse wheel with the right button held | flying speed |
| Left click | select the model under the pointer |
| F | go to the selected model |
| -, + | a narrower or a wider field of view than the game's |
| R | back to the game's camera and its field of view |
| 1, 2, 3 | textured, without textures, wire frame |
| B | the faces the game's camera could not see, on or off |
| H | the HUD (the 2D layer in front and the 3D gauges), on or off |
| K | the 2D backdrop, on or off |
| V | one viewport at a time, then all of them again |
| T | the texture pack, on or off |
| X | save the selected model's textures to `out\viewer_textures` |
| O | open another snapshot |
| Tab | the side panel, on or off |
| F12 | screenshot (`out\viewer_<frame>_<n>.bmp`) |
| F11 | full screen |
| Esc | let go of the selection; with nothing selected, close |

The camera works the way Maya's does (Alt and the three buttons) and the way
a game's free camera does (the right button and W A S D), whichever comes to
hand. An orbit turns about the selected model once **F** has put the camera on
it, and otherwise about whatever is in the middle of the picture.

The game films some shots (its attract sequence, for one) from far away
through a long lens, a few degrees wide. Flown with, such a lens shows next to no
perspective, so the first move away from the game's camera swaps it for a
normal one (55 degrees from top to bottom) and brings the camera forward to
where what was in the middle of the picture looks as large as it did. A lens
of 35 degrees or more is left as the game has it. **-** and **+** change it
by hand, and **R** goes back to the game's camera and lens.

The side panel shows the snapshot, the camera and, for the selected model, its
address (and whether it is in the ROM or in polygon RAM), its size, its
viewport and its textures. Each texture is listed under the name a texture pack gives it
(`textures\<name>.png`), which is also the name **X** saves it under, so a
saved texture can be edited and dropped into a pack as it is.

## What a snapshot does and does not hold

A snapshot is what the game drew in one frame, not the whole track:

- **The scene ends where the game stopped sending it.** The game only puts in
  the scene graph what is near the car, so flying far ahead reaches the edge
  of the world, and the horizon is a backdrop that does not move.
- **The faces behind.** The game's back-face test leaves out about half of
  every closed model. The viewer keeps the opaque ones and shows them (B), so
  models are whole from any side. Translucent faces that were left out are
  not kept.
- **The HUD's 3D parts stay put.** The viewports the game draws over the
  picture (the gauges, the countdown) do not follow the free camera; H hides
  them. The world and the player's car do.
- **The picture is the original's.** None of the game's extra effects
  (shadows, reflections, bloom, ray tracing) are on: several of them follow the
  game's camera from one frame to the next and have no meaning in a still.
- **Nothing moves.** One frame: no animation, no physics.
