// Sega Rally 2, DUST: ENHANCED (not in the original). Where a wheel throws up
// the road, the game draws a flat see-through fan of three stretches behind
// it (a cloud texture tinted with the ground's colour) and a row of five
// small sprites of flying bits. With the option on both are taken out and a
// particle system takes their place: a low, short-lived haze of dust that
// stays under the car's roof, in the ground's colour and lit by the sun, and
// solid dark lumps of earth thrown back and out from the tread that fall and
// bounce on the road.
#pragma once
#include <vector>

#include "r3d_scene.h"
#include "r3d_world.h"

// After R3DSceneBuilder::link_previous (the vertices it adds have no keys)
void r3d_sr2_dust(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances, R3DTextureRam* textures);
void r3d_set_sr2_dust(bool enhanced);
// The race is on the Snowy course: the dust is snow there (runtime.cpp, from the game's round)
void r3d_set_sr2_snowy(bool snowy);
