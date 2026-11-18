// The capture viewer's own geometry, apart from the game's renderer: a free
// camera over a built scene, what is drawn of it and what a click picks.
//
// R3DSceneBuilder leaves every vertex in the view space of the game's camera
// (the eye at the origin, looking down -Z, Y up) and the renderer draws them
// as they are. The viewer never changes the renderer: to look from somewhere
// else it moves a copy of the scene the other way (view_move) and hands that
// copy to R3DRenderer::set_scene. The same goes for the view modes: the wire
// frame is thin quads along the edges, built here (view_display).
//
// Nothing here needs Windows or Vulkan (tests/viewer_test.cpp runs it).
#pragma once
#include <cstdint>
#include <vector>

#include "r3d_scene.h"

// Where the 3D view is in the window: R3DRenderer's ScreenLayout
// (compute_layout), without its headers
struct ViewLayout {
    float view_w = 496;      // width of the 3D view in Model 3 pixels (496 = the original 4:3)
    float scale = 1;         // window pixels per Model 3 pixel
    float px_aspect = 1;     // how much wider than tall a Model 3 pixel is shown
    float ox = 0, oy = 0;    // window position of the 3D view
    float game_x0 = 0;       // Model 3 x of the 4:3 area inside the 3D view
};

// The free camera, in the capture's view space: at the origin and unturned it
// is the game's own camera
struct ViewCamera {
    float pos[3] = {0, 0, 0};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // its right, up and back axes
    // How much wider than the game's its view is: the tangent of its half
    // angle over the game's (2: twice as much of the scene at any distance)
    float lens = 1;

    void reset() { *this = ViewCamera{}; }
    bool moved() const;
    // Radians: yaw about `up` (the world's, so the horizon stays level), pitch about its own right axis
    void turn(const float* up, float yaw, float pitch);
    // Along its own right and forward axes, and along `up`
    void move(const float* up, float right, float rise, float forward);
    // Maya's tumble: `turn`, about the point `distance` in front of it, which stays in the middle of the view
    void orbit(const float* up, float yaw, float pitch, float distance);
    // Maya's track: along its own right and up axes
    void track(float right, float rise);
    void to_view(const float* point, float* out) const;   // a point of the capture as this camera sees it
    void rotate(const float* dir, float* out) const;      // a direction
};

// The world's up axis in the capture's view space ((0, 1, 0) without a world viewport)
void view_world_up(const R3DScene& scene, float up[3]);

// Which viewports the free camera moves, one flag per viewport: the game
// world's and the full-screen ones drawn with it (Sega Rally 2 gives the
// player's car one of its own). The rest are drawn over the picture (the
// HUD's 3D gauges, the countdown) and stay where the game put them. With no
// world viewport (menus) every one follows
std::vector<uint8_t> view_follows(const R3DScene& scene);

// `base` as `cam` sees it: the vertices, the culled faces, the lights and the
// projection of the viewports that follow
void view_move(const R3DScene& base, const ViewCamera& cam, const std::vector<uint8_t>& follows, R3DScene& out);

enum class ViewMode { Normal, Flat, Wire };

struct ViewOptions {
    ViewMode mode = ViewMode::Normal;
    bool backs = true;       // also the faces the game's back-face test left out (R3DScene::backs)
    bool overlays = true;    // also the viewports that do not follow the camera
    int solo = -1;           // only this viewport (index in the scene's list), -1: all
    int sel_viewport = -1;   // the selected model: its viewport (index in the scene's list)...
    int sel_instance = -1;   // ...and its place in R3DSceneBuilder::instances()[build_index]

    bool shows(size_t viewport, const std::vector<uint8_t>& follows) const {
        if (solo >= 0 && size_t(solo) != viewport) return false;
        return overlays || (viewport < follows.size() && follows[viewport]);
    }
};

// What the renderer is given: `moved` (view_move's) with the options applied
void view_display(const R3DScene& moved, const std::vector<std::vector<R3DInstance>>& instances,
                  const std::vector<uint8_t>& follows, const ViewOptions& opt, const ViewLayout& lay, R3DScene& out);

struct ViewPick {
    int viewport = -1;     // index in the scene's viewports, -1: nothing there
    int instance = -1;     // index in instances[build_index], -1: a polygon of no known model
    int list = 0;          // 0 opaque, 1 translucent, 2 culled faces (R3DScene::backs)
    uint32_t vertex = 0;   // the triangle's first vertex in that list
    float distance = 0;    // along the view axis
    float point[3] = {0, 0, 0};
    bool hit() const { return viewport >= 0; }
};

// The view ray through a point of the window, in the viewport's own view
// space (from the origin; z = -1). false outside the viewport
bool view_ray(const R3DViewport& v, const ViewLayout& lay, float win_x, float win_y, float dir[3]);

// The tangents of half the angle a viewport shows, across and down, in a 3D
// view of this layout (the renderer shows more to the sides of a full-screen
// viewport when the view is wider than 4:3)
void view_angles(const R3DViewport& v, const ViewLayout& lay, float& across, float& down);

// How much of the view one pixel of the window covers, at distance 1 from the
// camera (times the distance: the size of a pixel there)
float view_pixel_size(const R3DViewport& v, const ViewLayout& lay);

// The nearest polygon under a point of the window, as the picture shows it: a
// later priority covers the earlier ones
ViewPick view_pick(const R3DScene& moved, const std::vector<std::vector<R3DInstance>>& instances,
                   const std::vector<uint8_t>& follows, const ViewOptions& opt, const ViewLayout& lay, float win_x,
                   float win_y);

// A model's vertices: their middle and how far the farthest is from it
struct ViewBounds {
    float centre[3] = {0, 0, 0};
    float radius = 0;
    uint32_t vertices = 0;
};
ViewBounds view_bounds(const R3DScene& scene, const R3DViewport& v, const R3DInstance& in);
