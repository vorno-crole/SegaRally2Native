// World tracking for the atmosphere. The scenery is made of many models
// that share the world's orientation; their origins move only with the
// camera, which gives the camera position in a fixed world frame.
#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct R3DScene;

// Hash step shared by the scene builder's instance keys and the tracker
inline uint32_t r3d_mix_key(uint32_t h, uint32_t v) {
    h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h * 0x85EBCA6Bu;
}

// Two models turned the same way (their axes in view space)
inline bool r3d_same_axes(const float* a, const float* b) {
    for (int i = 0; i < 9; i++)
        if (std::fabs(a[i] - b[i]) > 0.01f) return false;
    return true;
}

// A model drawn in a viewport: where it stands in view space and which
// vertices of the viewport's lists are its
struct R3DInstance {
    uint32_t key;
    uint32_t model;    // model address
    float axes[9];     // model x, y, z axes in view space (normalised)
    float origin[3];   // model origin in view space
    uint32_t polygons;
    uint32_t opaque[2], trans[2];   // its vertices in the viewport's lists (first, end)
    uint32_t path = 0;              // the same for every copy of the model in the viewport
    uint32_t start = 0;             // model address (where its polygons start)
    uint32_t back[2] = {0, 0};      // its polygons the back-face test left out, in the viewport's list (first, end)
    // Where a copy of a model sits on the model it rides on (a driver's head
    // on its car's bodywork), in that model's axes, and that model (0: none)
    float ride[3] = {0, 0, 0};
    uint32_t ride_on = 0;
    // Its origin in the world (the world's viewport only), for pairing copies
    float world[3] = {0, 0, 0};
    bool has_world = false;
};

class R3DWorldTracker {
public:
    // Finds the viewport showing the game world among the instances drawn
    // this frame (one list per viewport, in build order), fills its world
    // fields and tags its vertices (R3D_WORLD, R3D_ACTOR)
    void track(const std::vector<std::vector<R3DInstance>>& viewport_instances, R3DScene& out);

private:
    struct Placement {   // a model in view space: origin, then its x, y, z axes
        float origin[3], axes[9];
    };
    struct Scenery {
        std::unordered_map<uint32_t, Placement> by_key, by_model;
    };
    Scenery scenery_prev_;   // last frame
    std::unordered_set<uint32_t> scenery_models_;   // model addresses of the last scenery tracked
    float camera_[3] = {}, velocity_[3] = {};
    float loop_offset_[3] = {};   // added to the camera for the atmosphere when the game loops its scenery
    int lost_ = 0;   // frames in a row the scenery could not be matched
    float ground_ = 0, far_ = 0;
    bool tracking_ = false;
};
