// SegaRally2Viewer: the capture viewer, first milestone of Nico Engine.
//
// Opens a debug snapshot of the game (F9: out/snap/snap_<frame>.bin) and
// shows it with the game's own scene builder and renderer, from a camera
// that moves freely: fly around the frame, see it untextured or as a wire
// frame, click a model to learn where it comes from and which textures it
// uses, and export those textures under the names a texture pack expects.
//
// A program of its own beside the game: it links none of the recompiled
// code, so it builds without the ROM, and it changes no file of the game.
// It needs what the game needs to draw a frame: data/vrom.bin (the models
// kept in ROM), read from the same folder the game uses.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <commdlg.h>
#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "panel.h"
#include "textures.h"
#include "view_math.h"

#include "image_io.h"
#include "r3d_dust.h"
#include "r3d_renderer.h"
#include "r3d_scene.h"
#include "r3d_state.h"
#include "text_hd.h"
#include "texture_pack.h"
#include "tilegen.h"
#include "ui_overlay.h"
#include "vk_presenter.h"

namespace {

const char* const APP_NAME = "Nico Engine capture viewer";
const char* const EXPORT_DIR = "out/viewer_textures";
constexpr float MAX_ASPECT = 16.0f / 9.0f;
constexpr uint32_t BACKDROP = 0xFF23272Du;   // behind the 3D image with the 2D layers off (BGRA, opaque)

struct Args {
    std::string capture;
    std::string data_dir, textures_dir;   // empty: data/ and textures/ here or next to the program
    int msaa = 4;
    bool help = false;
};

const char* const USAGE =
    "SegaRally2Viewer [snapshot.bin] [--data DIR] [--textures DIR] [--msaa N]\n"
    "\n"
    "  snapshot.bin    a snapshot saved by the game with F9 (out\\snap\\snap_<frame>.bin).\n"
    "                  Without it: the newest one in out\\snap, or a file dialog\n"
    "  --data DIR      the folder with vrom.bin (default: data, here or next to the program)\n"
    "  --textures DIR  the texture pack to look replacements up in (default: textures)\n"
    "  --msaa N        samples per pixel: 1, 2, 4 or 8 (default 4)\n";

bool file_exists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
// The folder of the program, with its trailing slash
std::string exe_folder() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    return s.substr(0, s.find_last_of("\\/") + 1);
}
std::string base_name(const std::string& path) { return path.substr(path.find_last_of("\\/") + 1); }

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size_t(size) : 0);
    bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok && !out.empty();
}

// The first of `name` here and next to the program that passes `exists`, or ""
template <typename Exists>
std::string find_beside(const std::string& name, Exists exists) {
    for (const std::string& path : {name, exe_folder() + name})
        if (exists(path)) return path;
    return "";
}

// The snapshot written last in a folder, or ""
std::string newest_snapshot(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA((dir + "/*.bin").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return "";
    std::string best;
    FILETIME best_time{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &best_time) > 0) {
            best = dir + "/" + fd.cFileName;
            best_time = fd.ftLastWriteTime;
        }
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return best;
}

// The Windows file dialog, on a thread of its own: it wants a single-threaded
// COM apartment, and this program's is multi-threaded (image_io.cpp)
std::string ask_for_capture(HWND owner) {
    std::string picked;
    std::thread dialog([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        char file[MAX_PATH] = "";
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = "Snapshots (*.bin)\0*.bin\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = "Open a snapshot (the game saves them with F9 in out\\snap)";
        std::string start = find_beside("out\\snap", dir_exists);
        if (!start.empty()) ofn.lpstrInitialDir = start.c_str();
        // (the current folder stays: data/ and textures/ are looked for in it)
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        if (GetOpenFileNameA(&ofn)) picked = file;
        CoUninitialize();
    });
    dialog.join();
    return picked;
}

void report(const std::string& message, bool error) {
    fprintf(error ? stderr : stdout, "%s\n", message.c_str());
    fflush(error ? stderr : stdout);
    if (error) MessageBoxA(nullptr, message.c_str(), APP_NAME, MB_OK | MB_ICONERROR);
}

std::string strf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// Alt is the key of Maya's camera here. Windows has other plans for it: let
// go alone it opens the window's menu, which then takes the next key, and
// with a letter it beeps for want of a menu with that letter. Both are
// stopped on their way to the window's own procedure (Alt+Space, Alt+F4 and
// Alt+Enter go on as before)
WNDPROC g_window_proc = nullptr;
LRESULT CALLBACK alt_filter(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
    if (msg == WM_SYSCHAR && wp != VK_SPACE) return 0;
    return CallWindowProcA(g_window_proc, hwnd, msg, wp, lp);
}

class Viewer {
public:
    int run(const Args& args);

private:
    bool open(const std::string& path);
    void build_scene();
    void compose_layers();
    void apply_layers();
    void refresh();
    ViewLayout layout() const;
    void select(const ViewPick& pick);
    const R3DInstance* selected() const;
    void focus();
    void next_viewport();
    void export_textures();
    bool key(WPARAM vk, bool repeat);
    void click(int x, int y);
    bool find_interest();
    bool take_camera();
    const R3DViewport* world_viewport() const;
    float pixel_size() const;
    void widen(float by);
    void poll(float dt);
    void draw_panel();
    void say(const std::string& message, bool warning = false) {
        status_ = message;
        status_warning_ = warning;
        panel_dirty_ = true;
        printf("%s\n", message.c_str());
        fflush(stdout);
    }

    VkPresenter presenter_;
    R3DRenderer renderer_;
    UiOverlay ui_;
    Panel panel_;
    bool ui_ok_ = false;

    std::vector<uint8_t> vrom_;
    std::string textures_dir_;
    R3DTextureRam ram_;
    TexturePack pack_;
    bool use_pack_ = false;

    Capture capture_;
    std::unique_ptr<R3DSceneBuilder> builder_;
    // The frame as the game's camera saw it, as the free camera sees it, and
    // what the renderer draws of that (the view mode, the selection)
    R3DScene base_, moved_, shown_;
    std::vector<uint8_t> follows_;
    float up_[3] = {0, 1, 0};
    size_t models_ = 0;

    // The 2D layers of the frame as composed, and as given to the renderer
    std::vector<uint32_t> back_full_ = std::vector<uint32_t>(BACK_W * SCREEN_H),
                          front_full_ = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_ = std::vector<uint32_t>(BACK_W * SCREEN_H),
                          front_ = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          text_cells_ = std::vector<uint32_t>(TextHD::ROW_TEXELS * TextHD::ROWS),
                          sky_coords_ = std::vector<uint32_t>(TileGen::SKY_COORDS_W * TileGen::SKY_COORDS_H);
    bool show_backdrop_ = true;

    ViewCamera cam_;
    ViewOptions opt_;
    float speed_ = 10.0f;   // units (metres) a second
    // What the mouse is doing to the camera while a button is held: looking
    // around (the right button), or Maya's three with Alt (left, middle, right)
    enum class Drag { None, Look, Orbit, Track, Dolly };
    Drag drag_ = Drag::None;
    int drag_button_ = 0;
    POINT drag_anchor_{};
    bool was_down_[3] = {};
    float interest_ = 10.0f;   // how far ahead the camera's centre of interest is (Maya's name): orbits turn about it
    float wheel_ = 0;          // notches turned since the last frame
    bool scene_dirty_ = true, panel_dirty_ = true, show_panel_ = true;
    int panel_w_ = 0, client_w_ = 0, client_h_ = 0;
    std::chrono::steady_clock::time_point panel_drawn_{};

    std::vector<TextureUse> textures_;   // of the selected model
    ViewBounds bounds_;
    std::string status_;
    bool status_warning_ = false;
    int shots_ = 0;
    std::string pending_open_;   // a file to open once the frame in hand is done
    bool ask_open_ = false;
};

ViewLayout Viewer::layout() const {
    ScreenLayout s = compute_layout(float(std::max(client_w_, 1)), float(std::max(client_h_, 1)), MAX_ASPECT, 1.0f);
    return {s.view_w, s.scale, s.px_aspect, s.ox, s.oy, s.game_x0};
}

const R3DInstance* Viewer::selected() const {
    if (!builder_ || opt_.sel_viewport < 0 || size_t(opt_.sel_viewport) >= base_.viewports.size()) return nullptr;
    const auto& lists = builder_->instances();
    uint32_t list = base_.viewports[size_t(opt_.sel_viewport)].build_index;
    if (list >= lists.size() || opt_.sel_instance < 0 || size_t(opt_.sel_instance) >= lists[list].size()) return nullptr;
    return &lists[list][size_t(opt_.sel_instance)];
}

// The frame's scene, built as the game builds it (a builder of its own each
// time: it keeps the previous frame to tell what moved, and there is none)
void Viewer::build_scene() {
    builder_ = std::make_unique<R3DSceneBuilder>();
    builder_->set_vrom(vrom_.data(), vrom_.size());
    builder_->set_texture_pack(use_pack_ ? &pack_ : nullptr, &ram_);
    builder_->build(*capture_.state, base_);
    std::vector<RgbaImage> images = pack_.take_new_images();
    if (!images.empty()) renderer_.add_replacements(images);
    follows_ = view_follows(base_);
    view_world_up(base_, up_);
    models_ = 0;
    for (const auto& list : builder_->instances()) models_ += list.size();
    scene_dirty_ = panel_dirty_ = true;
}

void Viewer::compose_layers() {
    std::fill(text_cells_.begin(), text_cells_.end(), 0u);
    std::fill(sky_coords_.begin(), sky_coords_.end(), 0u);
    TileGen::compose(capture_.vram.data(),
                     {.back = back_full_.data(), .front = front_full_.data(), .text_cells = text_cells_.data()});
}

// The 2D layers the renderer composes with the 3D image: the frame's own, or
// a plain backdrop below and nothing above
void Viewer::apply_layers() {
    if (show_backdrop_) back_ = back_full_;
    else std::fill(back_.begin(), back_.end(), BACKDROP);
    if (opt_.overlays) front_ = front_full_;
    else std::fill(front_.begin(), front_.end(), 0u);
    renderer_.set_text_cells(text_cells_.data());
    renderer_.set_sky_coords(sky_coords_.data());
    renderer_.set_layers(back_.data(), front_.data());
}

bool Viewer::open(const std::string& path) {
    Capture c;
    std::string error;
    if (!load_capture(path, c, error)) {
        report("Cannot open the snapshot: " + error, true);
        return false;
    }
    capture_ = std::move(c);
    ram_.load_all(capture_.texels);
    cam_.reset();
    opt_.sel_viewport = opt_.sel_instance = -1;
    opt_.solo = -1;
    textures_.clear();
    build_scene();
    compose_layers();
    apply_layers();
    SetWindowTextA(presenter_.hwnd(), (std::string(APP_NAME) + " - " + base_name(path)).c_str());
    size_t culled = base_.backs.size() / 3;
    say(strf("%s: frame %llu, %zu viewports, %zu models, %zu polygons (%zu triangles drawn, %zu culled)",
               base_name(path).c_str(), (unsigned long long)capture_.state->frame, base_.viewports.size(), models_,
               base_.polygons, base_.vertices.size() / 3, culled));
    for (size_t i = 0; i < base_.viewports.size(); i++) {
        const R3DViewport& v = base_.viewports[i];
        printf("  viewport %zu: priority %d, %.0fx%.0f at %.0f,%.0f, %u + %u triangles%s\n", i, v.priority, v.w, v.h, v.x,
               v.y, v.opaque_count / 3, v.trans_count / 3,
               v.world ? " (the game world)" : follows_[i] ? " (drawn with the world)" : "");
    }
    fflush(stdout);
    return true;
}

void Viewer::refresh() {
    view_move(base_, cam_, follows_, moved_);
    view_display(moved_, builder_->instances(), follows_, opt_, layout(), shown_);
    renderer_.set_scene(&shown_);
    scene_dirty_ = false;
}

void Viewer::select(const ViewPick& pick) {
    opt_.sel_viewport = opt_.sel_instance = -1;
    textures_.clear();
    scene_dirty_ = panel_dirty_ = true;
    if (!pick.hit()) {
        status_.clear();
        return;
    }
    if (pick.instance < 0) return say("That polygon belongs to no model", true);
    opt_.sel_viewport = pick.viewport;
    opt_.sel_instance = pick.instance;
    const R3DViewport& v = base_.viewports[size_t(pick.viewport)];
    const R3DInstance* in = selected();
    if (!in) return;
    bounds_ = view_bounds(base_, v, *in);
    textures_ = model_textures(base_, v, *in, ram_);
    for (TextureUse& t : textures_) t.in_pack = file_exists(textures_dir_ + "/" + texture_file_name(t.hash));
    say(strf("Model 0x%06X in viewport %d: %u polygons, %zu textures", in->start, pick.viewport, in->polygons,
               textures_.size()));
    for (const TextureUse& t : textures_)
        printf("  %s  %dx%d  format %u  at %d,%d  %u triangles%s\n", texture_file_name(t.hash).c_str(), t.word.w, t.word.h,
               t.word.format, t.word.x, t.word.y, t.triangles, t.in_pack ? "  (in the texture pack)" : "");
    fflush(stdout);
}

// The camera in front of the selected model, turned as it is
void Viewer::focus() {
    const R3DInstance* in = selected();
    if (!in) return say("Click a model first", true);
    if (!follows_[size_t(opt_.sel_viewport)]) return say("That model is drawn over the picture: the camera does not move it", true);
    take_camera();
    float away = std::max(bounds_.radius * 2.5f, 2.0f);
    for (int i = 0; i < 3; i++) cam_.pos[i] = bounds_.centre[i] + cam_.rot[6 + i] * away;
    scene_dirty_ = panel_dirty_ = true;
}

// All the viewports, then each one that draws anything alone
void Viewer::next_viewport() {
    int n = int(base_.viewports.size());
    for (int step = 0; step <= n; step++) {
        opt_.solo = opt_.solo + 1 >= n ? -1 : opt_.solo + 1;
        if (opt_.solo < 0) break;
        const R3DViewport& v = base_.viewports[size_t(opt_.solo)];
        if (v.opaque_count + v.trans_count > 0 && (opt_.overlays || follows_[size_t(opt_.solo)])) break;
    }
    scene_dirty_ = panel_dirty_ = true;
}

void Viewer::export_textures() {
    if (textures_.empty()) return say("Click a model with textures first", true);
    CreateDirectoryA("out", nullptr);
    CreateDirectoryA(EXPORT_DIR, nullptr);
    int saved = 0;
    for (const TextureUse& t : textures_)
        saved += png_save(std::string(EXPORT_DIR) + "/" + texture_file_name(t.hash), t.word.w, t.word.h, t.rgba.data());
    if (saved == int(textures_.size())) say(strf("%d textures saved to %s", saved, EXPORT_DIR));
    else say(strf("Only %d of %zu textures could be saved to %s", saved, textures_.size(), EXPORT_DIR), true);
}

// true: the key was the viewer's (Esc otherwise closes the window, F11 goes full screen)
bool Viewer::key(WPARAM vk, bool repeat) {
    bool lens_key = vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT;
    if (repeat && !lens_key) return vk != VK_ESCAPE && vk != VK_F11;
    switch (vk) {
    case '1': opt_.mode = ViewMode::Normal; break;
    case '2': opt_.mode = ViewMode::Flat; break;
    case '3': opt_.mode = ViewMode::Wire; break;
    case 'B': opt_.backs = !opt_.backs; break;
    case 'H':
        opt_.overlays = !opt_.overlays;
        if (opt_.solo >= 0 && !opt_.shows(size_t(opt_.solo), follows_)) opt_.solo = -1;
        apply_layers();
        break;
    case 'K':
        show_backdrop_ = !show_backdrop_;
        apply_layers();
        break;
    case 'T':
        if (!pack_.active()) {
            say("No texture pack in " + textures_dir_, true);
            break;
        }
        use_pack_ = !use_pack_;
        build_scene();   // (the same models in the same order: the selection stays)
        break;
    case 'V': next_viewport(); break;
    case 'R': cam_.reset(); break;
    case VK_OEM_PLUS: case VK_ADD: widen(1.1f); break;
    case VK_OEM_MINUS: case VK_SUBTRACT: widen(1 / 1.1f); break;
    case 'F': focus(); break;
    case 'X': export_textures(); break;
    case 'O': ask_open_ = true; break;
    case VK_TAB: show_panel_ = !show_panel_; break;
    case VK_F12: {
        CreateDirectoryA("out", nullptr);
        std::string path = strf("out/viewer_%llu_%d.bmp", (unsigned long long)base_.frame, ++shots_);
        presenter_.capture_next(path);
        say("Screenshot saved to " + path);
        break;
    }
    case VK_ESCAPE:
        if (!selected()) return false;   // nothing to let go of: the window closes
        select(ViewPick{});
        break;
    default: return false;
    }
    scene_dirty_ = panel_dirty_ = true;
    return true;
}

void Viewer::click(int x, int y) {
    if (show_panel_ && x < panel_w_) return;
    select(view_pick(moved_, builder_->instances(), follows_, opt_, layout(), float(x), float(y)));
}

// How far ahead the camera's centre of interest is (interest_): the selected
// model when the camera is on it (after F), or else whatever is in the middle
// of the picture. false with nothing there (the sky): as far as the last time
bool Viewer::find_interest() {
    const float NEAREST = 0.5f, FARTHEST = 5000.0f;
    if (selected() && follows_[size_t(opt_.sel_viewport)]) {
        float c[3];
        cam_.to_view(bounds_.centre, c);
        if (c[2] < 0 && std::hypot(c[0], c[1]) <= std::max(bounds_.radius, 0.5f)) {
            interest_ = std::clamp(-c[2], NEAREST, FARTHEST);
            return true;
        }
    }
    ViewOptions world = opt_;
    world.overlays = false;   // (the HUD's viewports do not move with the camera)
    ViewLayout lay = layout();
    float x = lay.ox + lay.view_w * lay.scale * lay.px_aspect / 2, y = lay.oy + SCREEN_H * lay.scale / 2;
    ViewPick pick = view_pick(moved_, builder_->instances(), follows_, world, lay, x, y);
    if (pick.hit()) interest_ = std::clamp(pick.distance, NEAREST, FARTHEST);
    return pick.hit();
}

// The first move away from the game's camera. The game films some shots (its
// attract sequence) from far away through a long lens, a few degrees wide:
// flown with, it shows next to no perspective (everything turns and slides as
// one flat picture). A normal lens takes its place, and the camera goes
// forward to where that lens shows what was in the middle of the picture as
// large as it was. true when it did (moved_ is then a frame behind)
bool Viewer::take_camera() {
    if (cam_.moved()) return false;
    const R3DViewport* v = world_viewport();
    if (!v) return false;
    float across = 0, down = 0;
    view_angles(*v, layout(), across, down);
    const float LONG = 0.3153f, NORMAL = 0.5206f;   // from top to bottom: under 35 degrees, and 55
    if (down <= 0 || down >= LONG) return false;
    cam_.lens = NORMAL / down;
    if (find_interest()) {
        float nearer = std::max(interest_ / cam_.lens, 0.5f);
        cam_.move(up_, 0, 0, interest_ - nearer);
        interest_ = nearer;
    }
    say(strf("Long lens of the game (%.0f deg) swapped: R undoes", std::atan(down) * 2 * 57.29578f));
    scene_dirty_ = true;
    return true;
}

// The viewport the camera's own numbers are told from: the world's as the
// free camera shows it, or the first one that follows it
const R3DViewport* Viewer::world_viewport() const {
    const R3DViewport* best = nullptr;
    for (size_t i = 0; i < moved_.viewports.size() && i < follows_.size(); i++)
        if (follows_[i] && (!best || moved_.viewports[i].world)) best = &moved_.viewports[i];
    return best;
}

// The size of a pixel of the window at distance 1 from the camera
float Viewer::pixel_size() const {
    const R3DViewport* v = world_viewport();
    float size = v ? view_pixel_size(*v, layout()) : 0.0f;
    return size > 0 ? size : 0.0015f;
}

// A wider or a narrower lens than the game's, between 5 and 140 degrees from
// top to bottom
void Viewer::widen(float by) {
    float across = 0, down = 0;
    if (const R3DViewport* v = world_viewport()) view_angles(*v, layout(), across, down);
    float lens = cam_.lens * by;
    if (down > 0) lens = cam_.lens * std::clamp(down * by, 0.0437f, 2.747f) / down;
    cam_.lens = std::clamp(lens, 0.05f, 20.0f);
}

// The camera. The right button held turns it with the mouse, W A S D move it
// along its own axes and Q E along the world's up. Maya's way too: with Alt,
// the left button orbits, the middle one slides the view and the right one
// goes nearer or farther, as the wheel does
void Viewer::poll(float dt) {
    HWND hwnd = presenter_.hwnd();
    bool focus = GetForegroundWindow() == hwnd;
    auto down = [&](int vk) { return focus && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    POINT p{};
    GetCursorPos(&p);

    // (GetAsyncKeyState tells the buttons as they are on the mouse: the other way round for a left-handed one)
    bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int buttons[3] = {swapped ? VK_RBUTTON : VK_LBUTTON, VK_MBUTTON, swapped ? VK_LBUTTON : VK_RBUTTON};
    const Drag with_alt[3] = {Drag::Orbit, Drag::Track, Drag::Dolly};
    for (int b = 0; b < 3; b++) {
        bool is_down = down(buttons[b]);
        if (is_down && !was_down_[b] && drag_ == Drag::None) {
            Drag wanted = down(VK_MENU) ? with_alt[b] : b == 2 ? Drag::Look : Drag::None;
            POINT c = p;
            RECT rc;
            ScreenToClient(hwnd, &c);
            GetClientRect(hwnd, &rc);
            if (wanted != Drag::None && PtInRect(&rc, c)) {
                drag_ = wanted;
                drag_button_ = buttons[b];
                drag_anchor_ = p;
                if (!take_camera() && drag_ != Drag::Look) find_interest();
                presenter_.set_cursor_visible(false);
            }
        }
        was_down_[b] = is_down;
    }
    if (drag_ != Drag::None && !down(drag_button_)) {
        drag_ = Drag::None;
        presenter_.set_cursor_visible(true);
    }
    if (drag_ != Drag::None && (p.x != drag_anchor_.x || p.y != drag_anchor_.y)) {
        const float per_pixel = 0.0025f;   // radians
        float dx = float(p.x - drag_anchor_.x), dy = float(p.y - drag_anchor_.y);
        switch (drag_) {
        case Drag::Look: cam_.turn(up_, -dx * per_pixel, -dy * per_pixel); break;
        case Drag::Orbit: cam_.orbit(up_, -dx * per_pixel, -dy * per_pixel, interest_); break;
        case Drag::Track: {   // what is at the centre of interest stays under the pointer
            float size = interest_ * pixel_size();
            cam_.track(-dx * size, dy * size);
            break;
        }
        case Drag::Dolly: {   // to the right or down: nearer, never past the centre of interest
            float nearer = std::clamp(interest_ * std::exp(-(dx + dy) * 0.005f), 0.2f, 5000.0f);
            cam_.move(up_, 0, 0, interest_ - nearer);
            interest_ = nearer;
            break;
        }
        case Drag::None: break;
        }
        SetCursorPos(drag_anchor_.x, drag_anchor_.y);   // (so it never reaches an edge of the screen)
        scene_dirty_ = true;
    }

    if (wheel_ != 0) {
        float notches = wheel_;
        wheel_ = 0;
        if (drag_ == Drag::Look) {   // flying: how fast
            speed_ = std::clamp(speed_ * std::pow(1.25f, notches), 0.5f, 400.0f);
        } else {
            // A part of the way to what is ahead, and no less than a short
            // step: close to a wall it would otherwise never get through
            if (!take_camera()) find_interest();
            float d = interest_, step = d * (1.0f - std::pow(0.85f, notches));
            if (notches > 0) step = std::max(step, 0.25f * notches);
            cam_.move(up_, 0, 0, step);
            interest_ = std::clamp(d - step, 0.5f, 5000.0f);
            scene_dirty_ = true;
        }
        panel_dirty_ = true;
    }

    float right = float(down('D')) - float(down('A')), rise = float(down('E')) - float(down('Q')),
          ahead = float(down('W')) - float(down('S'));
    if (right != 0 || rise != 0 || ahead != 0) {
        take_camera();
        float step = speed_ * dt * (down(VK_SHIFT) ? 4.0f : 1.0f) * (down(VK_CONTROL) ? 0.25f : 1.0f);
        cam_.move(up_, right * step, rise * step, ahead * step);
        scene_dirty_ = true;
    }
}

void Viewer::draw_panel() {
    panel_dirty_ = false;
    panel_drawn_ = std::chrono::steady_clock::now();
    if (!ui_ok_) return;
    if (!show_panel_ || client_w_ <= 0 || client_h_ <= 0) {
        panel_w_ = 0;
        ui_.hide();
        return;
    }
    const int font = std::clamp(client_h_ / 52, 13, 20);
    if (!panel_.begin(client_w_, client_h_, font)) return;
    const int lh = panel_.line_height(), pad = font;
    panel_w_ = std::min(client_w_, 34 * font);
    panel_.box(0, 0, panel_w_, client_h_);
    const uint32_t TITLE = 0xFFB040, INK = 0xD8DCE0, DIM = 0x8A9099, WARN = 0xFF7060, GOOD = 0x78D890;
    int y = pad;
    auto line = [&](const std::string& s, uint32_t color = 0xD8DCE0) {
        panel_.text(pad, y, s, color);
        y += lh;
    };
    // A setting, its value and the key that changes it
    auto setting = [&](const char* name, const std::string& value, const char* keys) {
        panel_.text(pad, y, name, DIM);
        panel_.text(pad + 9 * font, y, value, INK);
        panel_.text(panel_w_ - pad - panel_.text_width(keys), y, keys, TITLE);
        y += lh;
    };

    line("NICO ENGINE  capture viewer", TITLE);
    line(strf("Sega Rally 2  %s", base_name(capture_.path).c_str()));
    line(strf("frame %llu: %zu viewports, %zu models, %zu polygons", (unsigned long long)base_.frame,
                base_.viewports.size(), models_, base_.polygons),
         DIM);
    if (vrom_.empty()) line("No vrom.bin: the models kept in ROM are missing", WARN);
    y += lh / 2;
    setting("View", opt_.mode == ViewMode::Normal ? "as the game draws it" : opt_.mode == ViewMode::Flat ? "no textures" : "wire frame",
            "1 2 3");
    setting("Culled faces", opt_.backs ? "shown" : "hidden", "B");
    setting("HUD", opt_.overlays ? "shown" : "hidden", "H");
    setting("2D backdrop", show_backdrop_ ? "shown" : "hidden", "K");
    setting("Texture pack", !pack_.active() ? "none" : use_pack_ ? "on" : "off", "T");
    if (opt_.solo < 0) {
        setting("Viewports", "all", "V");
    } else {
        const R3DViewport& v = base_.viewports[size_t(opt_.solo)];
        setting("Viewports", strf("only %d (priority %d%s)", opt_.solo, v.priority, v.world ? ", world" : ""), "V");
    }
    setting("Camera", cam_.moved() ? strf("free, %.0f m/s", speed_) : "the game's", "R");
    if (const R3DViewport* v = world_viewport()) {
        float across, down;
        view_angles(*v, layout(), across, down);
        const float degrees = 2 * 57.29578f;
        setting("Field of view", strf("%.0f x %.0f deg%s", std::atan(across) * degrees, std::atan(down) * degrees,
                                      cam_.lens == 1 ? " (the game's)" : ""), "- +");
    }
    y += lh / 2;

    const int bottom = client_h_ - pad - 6 * lh;   // the help and the status line below
    if (const R3DInstance* in = selected()) {
        const R3DViewport& v = base_.viewports[size_t(opt_.sel_viewport)];
        line("SELECTED MODEL", TITLE);
        line(strf("0x%06X  %s", in->start, in->start < 0x100000 ? "polygon RAM" : "VROM"));
        line(strf("%u polygons, %u vertices, %.1f m across", in->polygons, bounds_.vertices, bounds_.radius * 2), DIM);
        line(strf("viewport %d, priority %d%s", opt_.sel_viewport, v.priority,
                    v.world ? " (the game world)" : follows_[size_t(opt_.sel_viewport)] ? " (drawn with the world)" : " (HUD)"),
             DIM);
        line(strf("instance %08X", in->key), DIM);
        y += lh / 2;
        line(textures_.empty() ? "No textures" : strf("TEXTURES (%zu)", textures_.size()), TITLE);
        const int thumb = 2 * lh + lh / 2;
        size_t drawn = 0;
        for (const TextureUse& t : textures_) {
            if (y + thumb + lh > bottom) break;
            panel_.image(pad, y, thumb, t.rgba.data(), t.word.w, t.word.h);
            int tx = pad + thumb + font / 2;
            panel_.text(tx, y, texture_file_name(t.hash), INK);
            panel_.text(tx, y + lh, strf("%dx%d  format %u  %u triangles", t.word.w, t.word.h, t.word.format, t.triangles), DIM);
            if (t.in_pack) {
                const char* tag = "in pack";
                panel_.text(panel_w_ - pad - panel_.text_width(tag), y + lh, tag, GOOD);
            }
            y += thumb + lh / 3;
            drawn++;
        }
        if (drawn < textures_.size()) line(strf("... and %zu more (X saves them all)", textures_.size() - drawn), DIM);
    } else {
        line("Click a model to select it", DIM);
    }

    y = bottom;
    line("Right button: look (wheel: speed)   W A S D Q E: fly", DIM);
    line("Alt + left, middle, right button: orbit, pan, zoom", DIM);
    line("Wheel: zoom   F: go to the model   Esc: let go", DIM);
    line("Shift: faster   Ctrl: slower   - +: field of view", DIM);
    line("X: save textures   O: open   F12: shot   Tab: panel", DIM);
    if (!status_.empty()) {
        // (what does not fit is in the console)
        std::string s = status_;
        size_t fit = size_t(std::max(8, (panel_w_ - 2 * pad) / std::max(1, panel_.text_width("0"))));
        if (s.size() > fit) s = s.substr(0, fit - 3) + "...";
        line(s, status_warning_ ? WARN : GOOD);
    }
    if (const uint32_t* picture = panel_.end()) ui_.set_image(picture, uint32_t(client_w_), uint32_t(client_h_));
}

int Viewer::run(const Args& args) {
    // The models kept in ROM: the same file the game reads
    std::string data_dir = !args.data_dir.empty() ? args.data_dir : find_beside("data", [](const std::string& d) {
        return file_exists(d + "/vrom.bin");
    });
    if (data_dir.empty() || !read_file(data_dir + "/vrom.bin", vrom_)) {
        vrom_.clear();
        report("No vrom.bin in " + (args.data_dir.empty() ? std::string("data\\ (here or next to the program)") : args.data_dir) +
                   ".\nIt is one of the files the game's build extracts from your own ROM (build.bat, or the launcher).\n"
                   "Without it only the models the game keeps in RAM are drawn. Give its folder with --data.",
               true);
    } else {
        printf("VROM: %s/vrom.bin (%zu MB)\n", data_dir.c_str(), vrom_.size() >> 20);
    }
    textures_dir_ = !args.textures_dir.empty() ? args.textures_dir : find_beside("textures", dir_exists);
    if (textures_dir_.empty()) textures_dir_ = "textures";

    std::string path = args.capture;
    if (path.empty()) path = newest_snapshot(find_beside("out/snap", dir_exists));
    if (path.empty()) path = ask_for_capture(nullptr);
    if (path.empty()) {
        report(std::string("No snapshot to open. In the game, F9 saves the frame on screen to out\\snap.\n\n") + USAGE, true);
        return 1;
    }

    if (!presenter_.init(APP_NAME, 1280, 720, false)) {
        report("Could not start Vulkan. Update the graphics driver: the viewer needs Vulkan 1.3, as the game does.", true);
        return 1;
    }
    printf("GPU: %s\n", presenter_.device_name().c_str());
    g_window_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(presenter_.hwnd(), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(alt_filter)));
    if (!renderer_.init(presenter_, 1.0f, args.msaa, 8)) {
        report("Could not start the 3D renderer: the graphics card or its driver lacks something it needs.", true);
        return 1;
    }
    // The picture as the arcade board drew it: none of the game's extra
    // effects, which follow the game's camera from frame to frame
    renderer_.set_max_aspect(MAX_ASPECT);
    renderer_.set_pixel_aspect(1.0f);
    renderer_.set_smooth_2d(true);
    renderer_.set_hud_edges(false);
    renderer_.set_hud_band(false);
    renderer_.set_distance_haze(0);
    renderer_.set_bloom(0);
    renderer_.set_picture(1, 1);
    renderer_.set_ambient_occlusion(0);
    renderer_.set_sun(false);
    renderer_.set_shadows(0);
    renderer_.set_ray_tracing(0);
    renderer_.set_soft_particles(false);
    renderer_.set_hdr(false);
    renderer_.set_reflections(false);
    renderer_.set_global_illumination(0);
    renderer_.set_sharpen(0);
    renderer_.set_interpolation(1.0f);
    renderer_.set_back_shift(0.0f);
    r3d_set_sr2_dust(false);   // (its particles live from frame to frame: the game's own dust is drawn)
    pack_.init(textures_dir_, "", "");
    pack_.set_max_slots(renderer_.texture_slots());
    printf("Texture pack: %s (%s)\n", textures_dir_.c_str(), pack_.active() ? "found" : "none");
    ui_ok_ = ui_.init(presenter_);
    if (!ui_ok_) printf("The side panel could not be started: the console has the same information\n");
    presenter_.set_cursor_visible(true);

    if (!open(path)) return 1;

    presenter_.input_hook = [this](UINT msg, WPARAM wp, LPARAM lp) -> bool {
        switch (msg) {
        case WM_KEYDOWN: return key(wp, (lp & (1 << 30)) != 0);
        case WM_LBUTTONDOWN:
            if (GetKeyState(VK_MENU) >= 0) click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));   // (with Alt it is an orbit)
            return true;
        case WM_MOUSEWHEEL:
            wheel_ += float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;   // (poll moves the camera)
            return true;
        }
        return false;
    };

    auto before = std::chrono::steady_clock::now();
    std::vector<uint16_t> rows;
    while (presenter_.pump()) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::min(std::chrono::duration<float>(now - before).count(), 0.1f);
        before = now;

        if (ask_open_) {   // (outside the window procedure: the dialog runs its own message loop)
            ask_open_ = false;
            pending_open_ = ask_for_capture(presenter_.hwnd());
        }
        if (!pending_open_.empty()) {
            std::string next;
            next.swap(pending_open_);
            open(next);   // (one that cannot be read leaves the frame in hand)
        }

        RECT rc;
        GetClientRect(presenter_.hwnd(), &rc);
        if (rc.right != client_w_ || rc.bottom != client_h_) {
            client_w_ = rc.right, client_h_ = rc.bottom;
            scene_dirty_ = panel_dirty_ = true;   // (the wire frame's lines are a pixel wide at any size)
        }
        const bool was_free = cam_.moved();
        poll(dt);
        if (cam_.moved() != was_free) panel_dirty_ = true;
        if (scene_dirty_) refresh();
        // (redrawn a few times a second at most: it is a picture of the whole window)
        if (panel_dirty_ && now - panel_drawn_ > std::chrono::milliseconds(120)) draw_panel();

        int y0 = 0, y1 = 0;
        if (ram_.take_dirty(rows, y0, y1)) renderer_.upload_texture_rows(rows, y0, y1);
        presenter_.frame([&](VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
            renderer_.record(cmd, view, extent);
            if (ui_ok_) ui_.record(cmd, view, extent);
        });
    }
    if (ui_ok_) ui_.shutdown();
    renderer_.shutdown();
    presenter_.shutdown();
    return 0;
}

bool parse_args(int argc, char** argv, Args& args, std::string& error) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                error = a + " needs a value";
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (a == "--help" || a == "-h" || a == "/?") {
            args.help = true;
        } else if (a == "--data") {
            if (!value(args.data_dir)) return false;
        } else if (a == "--textures") {
            if (!value(args.textures_dir)) return false;
        } else if (a == "--msaa") {
            std::string n;
            if (!value(n)) return false;
            args.msaa = std::clamp(atoi(n.c_str()), 1, 8);
        } else if (a.rfind("--", 0) == 0) {
            error = "unknown option " + a;
            return false;
        } else if (args.capture.empty()) {
            args.capture = a;
        } else {
            error = "one snapshot at a time";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    std::string error;
    if (!parse_args(argc, argv, args, error)) {
        fprintf(stderr, "%s\n\n%s", error.c_str(), USAGE);
        return 2;
    }
    if (args.help) {
        printf("%s", USAGE);
        return 0;
    }
    // On the heap: the 3D renderer alone is larger than is comfortable on the stack
    auto viewer = std::make_unique<Viewer>();
    return viewer->run(args);
}
