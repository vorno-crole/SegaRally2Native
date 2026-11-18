Pictures of OPTIONS > WHEEL (the wheel, the pedals and the hand brake), drawn for this project.

Loaded by src/ui/frontend.cpp (Frontend::wheel_art); a missing file falls back to the drawn one.
The sources are in src/ (SVG). To make the PNGs again: `node render.js wheel:512:512 gas:200:400 brake:200:400 handbrake:200:400 handbrake_base:200:400` in src/ (Playwright), then copy them here.
Fixed points the code relies on: wheel.png's rim is 228 px out from its centre; handbrake.png turns about (100, 350); the plates of gas.png and brake.png span 110-382 and 200-372 of 400.
