// Viewer: fly/walk camera over the reference terrain. Platform-independent; the platform layer fills ViewerInput.

typedef enum {
    CMD_NONE,
    CMD_TOGGLE_WALK,
    CMD_DEBUG_SHADED, CMD_DEBUG_LOD, CMD_DEBUG_CONTOUR, CMD_DEBUG_NORMALS,
    CMD_TOGGLE_WIREFRAME,
    CMD_LOD_FINER, CMD_LOD_COARSER,
    CMD_TOGGLE_VSYNC,
    CMD_SCREENSHOT,
    CMD_PRINT_CAMERA,
    CMD_TOGGLE_TREES, CMD_TOGGLE_COVER, CMD_TOGGLE_COVER_WIRE,
    CMD_TREE_DIST_LESS, CMD_TREE_DIST_MORE,
    CMD_TOGGLE_OVERLAY,
    CMD_CAM_LYING, CMD_CAM_STANDING, CMD_CAM_FREE, CMD_STANCE_CROUCH,
    CMD_COVER_DIST_LESS, CMD_COVER_DIST_MORE,
    CMD_TOGGLE_TAA, CMD_TOGGLE_SHADOWS,
    CMD_KEY_5, CMD_KEY_6, CMD_KEY_7, CMD_KEY_8,
    CMD_GALLERY, CMD_GALLERY_ROWS_LESS, CMD_GALLERY_ROWS_MORE, CMD_GALLERY_UP, CMD_GALLERY_DOWN, CMD_GALLERY_HOME,
    CMD_GALLERY_END, CMD_GALLERY_PAUSE, CMD_GALLERY_LEFT, CMD_GALLERY_RIGHT, CMD_GALLERY_REP,
} ViewerCommand;

typedef struct {
    bool forward, back, left, right, up, down, fast, slow;   // held
    f32 mouse_dx, mouse_dy;     // pixels since last frame, only while the mouse is captured
    f32 wheel;                  // notches
    ViewerCommand commands[16];
    u32 command_count;
} ViewerInput;

typedef struct {
    f64 pos[3];                 // local metres: x east, y altitude, z north; (0, *, 0) = terrain sample (0,0)
    f32 yaw, pitch;             // radians; yaw 0 = north, +pi/2 = east
    f32 speed;                  // m/s
    bool walk;
    u32 stance;                 // STANCE_*, walk mode only
    f32 eye;                    // walk mode: current eye height above ground (m), eases towards the stance's
    bool landing;               // walk mode entered from free flight: gliding down to the stance's eye height
} Camera;

enum { STANCE_STAND, STANCE_CROUCH, STANCE_PRONE };
static const f32 STANCE_EYE[3] = {1.75f, 1.0f, 0.35f};     // m above ground
static const f32 STANCE_SPEED[3] = {1.5f, 1.0f, 0.4f};    // m/s

#define CAMERA_MIN_CLEARANCE 0.5f
#define CAMERA_FOV_Y (60.0f * 3.14159265f / 180.0f)
#define CAMERA_NEAR 0.05f           // m; prone eyes are 0.35 m above the ground, among the ground cover

typedef struct {
    Vk vk;
    Terrain terrain;
    Vegetation veg;
    GroundCover cover;
    Shadows shadows;
    Overlay overlay;
    Gallery gallery;
    Camera cam;
    u32 debug_mode;
    bool wireframe;
    bool show_trees, show_cover, show_shadows;
    bool cover_wire;            // ground cover as wireframe
    f32 tree_dist;              // m
    f32 cover_dist;             // ground-cover distance factor: scales all its ranges (meshes, impostors, terrain hand-over)
    f32 target_px;              // terrain triangle edge target (pixels)
    f32 time;
    // TAA: jitter sequence index; previous frame's camera (absolute) for motion vectors
    bool taa;
    VkPipeline pipe_taa;
    u32 taa_frame;
    bool prev_valid;
    f64 prev_pos[3];
    v3 prev_right, prev_up, prev_fwd;
    f32 prev_px, prev_py, prev_time;
    // Stats
    u32 node_count;
    f32 gpu_ms;                 // whole frame
    f32 gpu_pass_ms[GPU_PASSES];    // shadows, terrain, trees, ground cover, post (sky + TAA)
    const char* screenshot_path;
    u32 screenshot_counter;
    bool swapchain_dirty;
    bool last_frame_world;      // the last rendered frame showed the world (gallery background source)
} Viewer;

static void camera_basis(const Camera* c, v3* right, v3* up, v3* fwd)
{
    *fwd = v3_make(sinf(c->yaw) * cosf(c->pitch), sinf(c->pitch), cosf(c->yaw) * cosf(c->pitch));
    *right = v3_norm(v3_cross(v3_make(0, 1, 0), *fwd));
    *up = v3_cross(*fwd, *right);
}

static void viewer_print_camera(const Viewer* v)
{
    const Camera* c = &v->cam;
    printf("--cam %.2f %.2f %.2f %.1f %.1f\n", v->terrain.hdr.origin_e + c->pos[0], v->terrain.hdr.origin_n + c->pos[2], c->pos[1],
           c->yaw * 180.0 / 3.14159265, c->pitch * 180.0 / 3.14159265);
}

static void gallery_toggle(Viewer* v)
{
    Gallery* g = &v->gallery;
    if (!g->active && !g->enabled) return;
    g->active = !g->active;
    if (g->active) {
        g->overlay_was_visible = v->overlay.visible;
        v->overlay.visible = false;
        v->vk.frozen_valid = false;
        g->prev_angle = g->angle;
    } else {
        v->overlay.visible = g->overlay_was_visible;
    }
    v->vk.history_valid = false;
}

// Gallery mode: its own keys; no camera movement.
static void gallery_update(Viewer* v, const ViewerInput* in, f32 dt)
{
    Gallery* g = &v->gallery;
    if (!g->paused) g->angle = fmodf(g->angle + GALLERY_SPIN * dt, 6.2831853f);
    for (u32 i = 0; i < in->command_count; i++) {
        switch (in->commands[i]) {
        case CMD_GALLERY: gallery_toggle(v); return;
        case CMD_GALLERY_ROWS_LESS: g->rows = MAX(g->rows - 1, 1u); break;
        case CMD_GALLERY_ROWS_MORE: g->rows = MIN(g->rows + 1, 6u); break;
        case CMD_GALLERY_UP: g->row--; break;
        case CMD_GALLERY_DOWN: g->row++; break;
        case CMD_GALLERY_HOME: g->row = 0; break;
        case CMD_GALLERY_END: g->row = 1 << 20; break;           // clamped by gallery_layout
        case CMD_GALLERY_PAUSE: g->paused = !g->paused; break;
        case CMD_GALLERY_LEFT: g->angle -= 0.2618f; break;
        case CMD_GALLERY_RIGHT: g->angle += 0.2618f; break;
        case CMD_GALLERY_REP: g->rep = (g->rep + 1) % 5; break;
        case CMD_DEBUG_SHADED: g->mode = 1; break;
        case CMD_DEBUG_LOD: g->mode = 2; break;
        case CMD_DEBUG_CONTOUR: g->mode = 3; break;
        case CMD_DEBUG_NORMALS: g->mode = 4; break;
        case CMD_KEY_5: g->mode = 5; break;
        case CMD_KEY_6: g->mode = 6; break;
        case CMD_KEY_7: g->mode = 7; break;
        case CMD_KEY_8: g->mode = 8; break;
        case CMD_TOGGLE_OVERLAY: v->overlay.visible = !v->overlay.visible; break;
        case CMD_TOGGLE_TAA: v->taa = !v->taa; v->vk.history_valid = false; break;
        case CMD_TOGGLE_VSYNC: v->vk.vsync = !v->vk.vsync; v->swapchain_dirty = true; break;
        case CMD_SCREENSHOT: v->screenshot_path = "shot"; break;
        default: break;
        }
    }
    g->row -= (i32)in->wheel;
}

static void viewer_update(Viewer* v, const ViewerInput* in, f32 dt)
{
    Camera* c = &v->cam;
    v->time += dt;
    if (v->gallery.active) { gallery_update(v, in, dt); return; }
    for (u32 i = 0; i < in->command_count; i++) {
        switch (in->commands[i]) {
        case CMD_GALLERY: gallery_toggle(v); return;
        // Camera presets: lying, standing (and crouching) on the ground, free flight. Every change glides: the eye height
        // eases between stances, and from free flight the camera descends to the ground instead of jumping.
        case CMD_TOGGLE_WALK:
            if (c->walk) { c->walk = false; c->speed = 15.0f; }
            else { c->walk = c->landing = true; c->eye = STANCE_EYE[c->stance]; c->speed = STANCE_SPEED[c->stance]; }
            break;
        case CMD_CAM_LYING:
        case CMD_CAM_STANDING:
        case CMD_STANCE_CROUCH: {
            ViewerCommand cmd = in->commands[i];
            u32 s = cmd == CMD_CAM_LYING ? STANCE_PRONE : cmd == CMD_CAM_STANDING ? STANCE_STAND
                  : c->stance == STANCE_CROUCH ? STANCE_STAND : STANCE_CROUCH;      // C toggles crouching
            if (!c->walk) { c->walk = c->landing = true; c->eye = STANCE_EYE[s]; }
            c->stance = s;
            c->speed = STANCE_SPEED[s];
            break;
        }
        case CMD_CAM_FREE:
            if (c->walk) { c->walk = false; c->speed = 15.0f; }
            break;
        case CMD_DEBUG_SHADED: v->debug_mode = DEBUG_SHADED; break;
        case CMD_DEBUG_LOD: v->debug_mode = DEBUG_LOD; break;
        case CMD_DEBUG_CONTOUR: v->debug_mode = DEBUG_CONTOUR; break;
        case CMD_DEBUG_NORMALS: v->debug_mode = DEBUG_NORMALS; break;
        case CMD_TOGGLE_WIREFRAME: v->wireframe = !v->wireframe && v->terrain.pipeline_wire; break;
        case CMD_LOD_FINER: v->target_px = MAX(v->target_px / 1.25f, 1.0f); break;
        case CMD_LOD_COARSER: v->target_px = MIN(v->target_px * 1.25f, 64.0f); break;
        case CMD_TOGGLE_VSYNC: v->vk.vsync = !v->vk.vsync; v->swapchain_dirty = true; break;
        case CMD_SCREENSHOT: v->screenshot_path = "shot"; break;
        case CMD_PRINT_CAMERA: viewer_print_camera(v); break;
        case CMD_TOGGLE_TREES: v->show_trees = !v->show_trees; break;
        case CMD_TOGGLE_COVER: v->show_cover = !v->show_cover; break;
        case CMD_TOGGLE_COVER_WIRE: v->cover_wire = !v->cover_wire && v->cover.wire; break;
        case CMD_TREE_DIST_LESS: v->tree_dist = MAX(v->tree_dist / 1.25f, 100.0f); break;
        case CMD_TREE_DIST_MORE: v->tree_dist = MIN(v->tree_dist * 1.25f, 12000.0f); break;
        case CMD_COVER_DIST_LESS: v->cover_dist = MAX(v->cover_dist / 1.25f, 0.5f); break;
        case CMD_COVER_DIST_MORE: v->cover_dist = MIN(v->cover_dist * 1.25f, 8.0f); break;
        case CMD_TOGGLE_OVERLAY: v->overlay.visible = !v->overlay.visible; break;
        case CMD_TOGGLE_TAA: v->taa = !v->taa; v->vk.history_valid = false; break;
        case CMD_TOGGLE_SHADOWS: v->show_shadows = !v->show_shadows; break;
        default: break;
        }
    }

    const f32 sensitivity = 0.0022f;
    c->yaw = fmodf(c->yaw + in->mouse_dx * sensitivity, 2.0f * 3.14159265f);
    c->pitch = CLAMP(c->pitch - in->mouse_dy * sensitivity, -1.55f, 1.55f);
    c->speed = CLAMP(c->speed * powf(1.25f, in->wheel), 0.25f, 3000.0f);

    v3 right, up, fwd;
    camera_basis(c, &right, &up, &fwd);
    if (c->walk) fwd = v3_norm(v3_make(fwd.x, 0, fwd.z));
    v3 move = {};
    if (in->forward) move = v3_add(move, fwd);
    if (in->back) move = v3_sub(move, fwd);
    if (in->right) move = v3_add(move, right);
    if (in->left) move = v3_sub(move, right);
    if (!c->walk && in->up) move.y += 1;
    if (!c->walk && in->down) move.y -= 1;
    if (v3_dot(move, move) > 0) move = v3_norm(move);
    f32 speed = c->speed * (in->fast ? 8.0f : 1.0f) * (in->slow ? 0.125f : 1.0f);
    c->pos[0] += move.x * speed * dt;
    c->pos[1] += move.y * speed * dt;
    c->pos[2] += move.z * speed * dt;

    f32 ground = terrain_height(&v->terrain, c->pos[0], c->pos[2]);
    c->eye += (STANCE_EYE[c->stance] - c->eye) * MIN(dt * 4.0f, 1.0f);   // ~0.7 s between standing and lying
    if (c->walk) {
        f64 target = ground + c->eye;
        if (c->landing) {                                                 // from free flight: glide down (~1 s)
            c->pos[1] += (target - c->pos[1]) * MIN(dt * 4.0f, 1.0f);
            if (fabs(target - c->pos[1]) < 0.02) c->landing = false;
        } else {
            c->pos[1] = target;
        }
    }
    else c->pos[1] = MAX(c->pos[1], (f64)ground + CAMERA_MIN_CLEARANCE);
}

static void write_ppm(const char* path, const u8* bgra_or_rgba, u32 w, u32 h, bool bgra)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    u8* row = (u8*)malloc((size_t)w * 3);
    for (u32 y = 0; y < h; y++) {
        const u8* src = bgra_or_rgba + (size_t)y * w * 4;
        for (u32 x = 0; x < w; x++) {
            row[3 * x + 0] = src[4 * x + (bgra ? 2 : 0)];
            row[3 * x + 1] = src[4 * x + 1];
            row[3 * x + 2] = src[4 * x + (bgra ? 0 : 2)];
        }
        fwrite(row, 3, w, f);
    }
    free(row);
    fclose(f);
    printf("wrote %s\n", path);
}

// Overlay boxes for this frame: [0] key bar along the bottom (always), then the performance panel (if visible), then the
// gallery cell labels. Returns the number of boxes; the gallery layout is computed here (it needs the bar's height).
static u32 viewer_overlay(Viewer* v, OverlayData* boxes)
{
    static const char* VIEW_NAMES[] = {"SHADED", "LOD", "CONTOURS", "NORMALS"};
    const Overlay* o = &v->overlay;
    Gallery* g = &v->gallery;
    u32 w = v->vk.extent.width, h = v->vk.extent.height, n = 0;
    const char* on[2] = {"OFF", "ON"};
    static KeyBar bar;
    keybar_begin(&bar, w, h);
    if (g->active) {
        u32 total = gallery_count(g), cols = MAX(g->cols, 1u), pages_rows = (total + cols - 1) / cols;
        keybar_add(&bar, "F2", "WORLD");
        keybar_add(&bar, "PGUP/PGDN", "ROWS %u", g->rows);
        keybar_add(&bar, "UP/DOWN WHEEL", "SCROLL (ROW %d/%u)", g->row + 1, pages_rows);
        keybar_add(&bar, "HOME/END", "FIRST/LAST");
        keybar_add(&bar, "1-8", "VIEW %s", GALLERY_MODE_NAMES[g->mode]);
        keybar_add(&bar, "R", "REPRESENTATION %s", GALLERY_REP_NAMES[g->rep]);
        keybar_add(&bar, "SPACE", "ROTATION %s", g->paused ? "PAUSED" : "ON");
        keybar_add(&bar, "LEFT/RIGHT", "TURN");
        keybar_add(&bar, "J", "TAA %s", on[v->taa]);
        keybar_add(&bar, "V", "VSYNC %s", on[v->vk.vsync]);
        keybar_add(&bar, "H", "PERFORMANCE %s", on[o->visible]);
        keybar_add(&bar, "K/F12", "SCREENSHOT");
        keybar_add(&bar, "ESC", "MOUSE / QUIT");
        keybar_add(&bar, "VIEWS", "1 FINAL 2 TEXTURES 3 WIRE 4 WIRE+CULL 5 NORMALS 6 ALPHA 7 UV 8 OVERDRAW");
    } else {
        const Camera* c = &v->cam;
        keybar_add(&bar, "CLICK/ESC", "MOUSE LOOK / RELEASE, QUIT");
        keybar_add(&bar, "WASD", "MOVE");
        keybar_add(&bar, "Q/E SPACE", "DOWN/UP");
        keybar_add(&bar, "SHIFT/CTRL", "X8/X1/8");
        keybar_add(&bar, "WHEEL", "SPEED %.1f M/S", c->speed);
        keybar_add(&bar, "Z/X/F", "LIE/STAND/FREE (%s)", !c->walk ? "FREE" : c->stance == STANCE_PRONE ? "LYING" : c->stance == STANCE_CROUCH ? "CROUCHING" : "STANDING");
        keybar_add(&bar, "C", "CROUCH");
        keybar_add(&bar, "G", "WALK/FREE");
        keybar_add(&bar, "T", "TREES %s", on[v->show_trees]);
        keybar_add(&bar, "B", "GROUND COVER %s", !v->cover.enabled ? "N/A" : on[v->show_cover]);
        keybar_add(&bar, "-/=", "TREE DIST %.0f M", v->tree_dist);
        keybar_add(&bar, ",/.", "COVER DIST X%.2f", v->cover_dist);
        keybar_add(&bar, "1-4", "VIEW %s", VIEW_NAMES[v->debug_mode & 3]);
        keybar_add(&bar, "L", "TERRAIN WIRE %s", on[v->wireframe]);
        keybar_add(&bar, "N", "COVER WIRE %s", on[v->cover_wire]);
        keybar_add(&bar, "[/]", "TERRAIN LOD %.1f PX", v->target_px);
        keybar_add(&bar, "O", "SHADOWS %s", !v->shadows.enabled ? "N/A" : on[v->show_shadows]);
        keybar_add(&bar, "J", "TAA %s", on[v->taa]);
        keybar_add(&bar, "V", "VSYNC %s", on[v->vk.vsync]);
        keybar_add(&bar, "H", "PERFORMANCE %s", on[o->visible]);
        keybar_add(&bar, "K/F12", "SCREENSHOT");
        keybar_add(&bar, "P", "PRINT CAMERA");
        keybar_add(&bar, "F2", "PLANT GALLERY");
    }
    f32 bar_px = keybar_end(o, &bar, &boxes[n++], w, h);
    if (o->visible) {
        OverlayData* d = &boxes[n++];
        overlay_begin(o, d, w, h);
        overlay_line(d, 0, NULL, "%.1f FPS  %.2f MS", o->fps, o->fps > 0 ? 1000.0f / o->fps : 0.0f);
        overlay_line(d, 1, NULL, "GPU %.2f MS", v->gpu_ms);
        char legend[OVERLAY_PANEL_COLS + 1];
        overlay_graph_legend(legend, OVERLAY_PANEL_COLS);
        overlay_line(d, 10, NULL, "%s", legend);
        overlay_line(d, 11, NULL, "FPS, TOP %.0f, LINES 30/60", d->params.y);
    }
    if (g->active) {
        gallery_layout(g, w, h, bar_px);
        f32 s = overlay_texel(h);
        u32 cols = MIN((u32)((g->cell_px - 8 * s) / (6 * s)), 60u);
        for (u32 k = 0; k < g->cells && n < OVERLAY_MAX_BOXES; k++) {
            GalleryElement el;
            gallery_element(g, &v->cover, &v->veg, (u32)g->row * g->cols + k, &el);
            OverlayData* d = &boxes[n++];
            f32 x = (f32)(g->x0 + (k % g->cols) * g->cell_px) + 4, y = (f32)(g->y0 + (k / g->cols + 1) * g->cell_px) - overlay_box_height(h, 2) - 4;
            overlay_box(o, d, w, h, x, y, cols, 2);
            overlay_line(d, 0, NULL, "%s", el.name);
            overlay_line(d, 1, NULL, "%s", el.rep);
        }
    }
    return n;
}
// Full-screen passes after the scene: TAA resolve (writes the swapchain image and the next history).
static void viewer_init_post(Viewer* v, const char* shader_dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%staa.spv", shader_dir);
    VkShaderModule m = vk_load_shader(&v->vk, path);
    PipelineDesc d = {m, "vs_taa", "fs_taa", VK_COMPARE_OP_ALWAYS, false, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL};
    d.color_count = 2;
    d.color_formats[0] = v->vk.swap_format;
    d.color_formats[1] = SCENE_FORMAT;
    v->pipe_taa = vk_create_pipeline(&v->vk, &d);
    vkDestroyShaderModule(v->vk.device, m, NULL);
}

// Records and submits one frame. Returns false if the swapchain must be recreated.
static bool viewer_render(Viewer* v)
{
    Vk* vk = &v->vk;
    Terrain* t = &v->terrain;
    VkFrame* f = &vk->frames[vk->frame_index % FRAMES_IN_FLIGHT];
    VK_CHECK(vkWaitForFences(vk->device, 1, &f->fence, VK_TRUE, UINT64_MAX));
    if (f->submitted && vk->timestamps) {
        u64 ts[GPU_TIMESTAMPS];
        if (vkGetQueryPoolResults(vk->device, f->queries, 0, GPU_TIMESTAMPS, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            f64 ms = vk->props.limits.timestampPeriod * 1e-6;
            v->gpu_ms = (f32)((ts[GPU_TIMESTAMPS - 1] - ts[0]) * ms);
            for (u32 i = 0; i < GPU_PASSES; i++) v->gpu_pass_ms[i] = (f32)((ts[i + 1] - ts[i]) * ms);
        }
    }

    u32 image;
    VkResult r = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) FATAL("vkAcquireNextImageKHR = %d", (int)r);
    VK_CHECK(vkResetFences(vk->device, 1, &f->fence));

    // Render origin: corner of the camera's 256 m cell (docs/02).
    const Camera* c = &v->cam;
    f64 origin[3];
    for (int i = 0; i < 3; i++) origin[i] = floor(c->pos[i] / 256.0) * 256.0;
    v3 cam = v3_make((f32)(c->pos[0] - origin[0]), (f32)(c->pos[1] - origin[1]), (f32)(c->pos[2] - origin[2]));
    v3 right, up, fwd;
    camera_basis(c, &right, &up, &fwd);
    f32 proj_y = 1.0f / tanf(CAMERA_FOV_Y * 0.5f);
    f32 proj_x = proj_y * (f32)vk->extent.height / (f32)vk->extent.width;
    const f32 sun_el = 35.0f * 3.14159265f / 180.0f, sun_az = 200.0f * 3.14159265f / 180.0f;

    FrameConstants* fc = (FrameConstants*)f->upload.mapped;
    memset(fc, 0, sizeof(*fc));
    fc->cam_pos = {cam.x, cam.y, cam.z, CAMERA_NEAR};
    fc->cam_right = {right.x, right.y, right.z, proj_x};
    fc->cam_up = {up.x, up.y, up.z, proj_y};
    fc->cam_fwd = {fwd.x, fwd.y, fwd.z, v->time};
    fc->sun_dir = {sinf(sun_az) * cosf(sun_el), sinf(sun_el), cosf(sun_az) * cosf(sun_el), 6e-5f};
    const TerrainFileHeader* h = &t->hdr;
    v3 terrain_origin = v3_make((f32)-origin[0], (f32)(h->height_min - origin[1]), (f32)-origin[2]);
    fc->terrain = {terrain_origin.x, terrain_origin.z, h->spacing, terrain_origin.y};
    fc->terrain_size = {(f32)h->width, (f32)h->height, h->height_scale * 65535.0f, 0};
    fc->height_tex = TEX_HEIGHT;
    fc->mask_tex = TEX_MASK;
    fc->debug_mode = v->debug_mode;
    fc->screen = {(f32)vk->extent.width, (f32)vk->extent.height, 1.0f / vk->extent.width, 1.0f / vk->extent.height};
    fc->viewport = {0, 0, (f32)vk->extent.width, (f32)vk->extent.height};
    Gallery* gal = &v->gallery;
    bool gallery = gal->active;
    bool gallery_history = gallery && gal->mode != 3 && gal->mode != 4 && gal->mode != 6 && gal->mode != 8;   // TAA history only where it helps
    u32 history_read = (u32)(vk->frame_index & 1), history_write = history_read ^ 1u;
    fc->taa = {v->prev_valid ? v->prev_time : v->time, v->taa && vk->history_valid && (!gallery || gallery_history) ? 0.1f : 1.0f, 0, 0};
    fc->history_tex = TEX_HISTORY + history_read;
    terrain_set_ranges(t, proj_y * vk->extent.height * 0.5f, v->target_px, fc);

    const VkDeviceSize nodes_offset = UPLOAD_NODES_OFFSET;
    static_assert(sizeof(FrameConstants) <= UPLOAD_VIEWS_OFFSET, "FrameConstants too large");
    static_assert(UPLOAD_VIEWS_OFFSET + MAX_VIEWS * sizeof(ViewConstants) <= UPLOAD_NODES_OFFSET, "too many views");
    static_assert(UPLOAD_NODES_OFFSET + TERRAIN_MAX_NODES * sizeof(TerrainNode) <= UPLOAD_BYTES, "upload buffer too small");
    ViewConstants* views = (ViewConstants*)((u8*)f->upload.mapped + UPLOAD_VIEWS_OFFSET);
    // TAA jitter: Halton (2, 3), 8 samples, in NDC. The previous view is unjittered and expressed relative to this
    // frame's render origin, so motion vectors stay valid across origin changes.
    f32 jx = 0, jy = 0, jx_px = 0, jy_px = 0;
    if (v->taa && (!gallery || gallery_history)) {
        u32 k = v->taa_frame % 8 + 1;
        f32 hx = 0, hy = 0;
        for (f32 b = 0.5f, i = (f32)k; i > 0; i = floorf(i / 2), b *= 0.5f) hx += b * fmodf(i, 2);
        for (f32 b = 1.0f / 3, i = (f32)k; i > 0; i = floorf(i / 3), b /= 3) hy += b * fmodf(i, 3);
        jx_px = hx - 0.5f;
        jy_px = hy - 0.5f;
        jx = jx_px * 2.0f / vk->extent.width;
        jy = jy_px * 2.0f / vk->extent.height;
    }
    ViewConstants prev;
    if (v->prev_valid) {
        v3 pc_ = v3_make((f32)(v->prev_pos[0] - origin[0]), (f32)(v->prev_pos[1] - origin[1]), (f32)(v->prev_pos[2] - origin[2]));
        view_perspective(&prev, pc_, v->prev_right, v->prev_up, v->prev_fwd, v->prev_px, v->prev_py, CAMERA_NEAR, 0, 0);
    } else {
        view_perspective(&prev, cam, right, up, fwd, proj_x, proj_y, CAMERA_NEAR, 0, 0);
    }
    view_perspective(&views[0], cam, right, up, fwd, proj_x, proj_y, CAMERA_NEAR, jx, jy);
    memcpy(views[0].prev_view_proj, prev.view_proj, sizeof(prev.view_proj));
    TerrainSelect sel = {};
    sel.cam = cam;
    sel.near_d = CAMERA_NEAR;
    sel.planes_n[0] = fwd;
    sel.planes_n[1] = v3_add(right, v3_scale(fwd, 1.0f / proj_x));
    sel.planes_n[2] = v3_sub(v3_scale(fwd, 1.0f / proj_x), right);
    sel.planes_n[3] = v3_add(up, v3_scale(fwd, 1.0f / proj_y));
    sel.planes_n[4] = v3_sub(v3_scale(fwd, 1.0f / proj_y), up);
    sel.origin_terrain = terrain_origin;
    sel.out = (TerrainNode*)((u8*)f->upload.mapped + nodes_offset);
    if (!gallery) terrain_select_node(t, &sel, t->levels - 1, 0, 0);
    v->node_count = sel.count;

    // Vegetation.
    Vegetation* vg = &v->veg;
    bool trees = vg->enabled && v->show_trees && !gallery;
    bool cover = v->cover.enabled && v->show_cover && !gallery;
    fc->veg = {v->tree_dist, v->cover_dist, proj_y * vk->extent.height * 0.5f, 0};
    fc->tree_lod = {400.0f, 120.0f, 30.0f, 0};
    fc->wind = {0.8f, 0.5f, 0.6f, 0};
    v3 gc = v->cover.enabled ? v->cover.grass_color : v3_make(0.11f, 0.15f, 0.05f);
    fc->cover = {gc.x, gc.y, gc.z, GC_GREENNESS};
    fc->veg_flags = (trees ? VEG_TREES : 0u) | (cover ? VEG_COVER : 0u);
    vg->chunk_count = vg->tree_candidates = 0;
    if (trees)
        veg_select_trees(vg, &sel, origin, c->pos, v->tree_dist, (TreeChunk*)((u8*)f->upload.mapped + CHUNKS_OFFSET));
    if (cover) gc_frame(&v->cover, c->pos, v->cover_dist, fc);
    bool shadows = v->shadows.enabled && v->show_shadows && !gallery;
    if (shadows)
        shadows_frame(&v->shadows, vg, views, fc, cam, fwd, proj_x, proj_y, CAMERA_NEAR, origin, c->pos, (TreeChunk*)((u8*)f->upload.mapped + CHUNKS_OFFSET), trees);
    u32 overlay_boxes = viewer_overlay(v, (OverlayData*)((u8*)f->upload.mapped + OVERLAY_OFFSET));

    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandPool(vk->device, f->pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    if (vk->timestamps) {
        vkCmdResetQueryPool(cmd, f->queries, 0, GPU_TIMESTAMPS);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, f->queries, 0);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipeline_layout, 0, 1, &vk->set, 0, NULL);
    PushConstants pcs = {f->upload.address, f->upload.address + nodes_offset, vg->scene.address, f->upload.address + CHUNKS_OFFSET,
                         f->upload.address + OVERLAY_OFFSET, f->upload.address + UPLOAD_VIEWS_OFFSET,
                         v->cover.scene.address};
    if (shadows)
        shadows_record(&v->shadows, vk, cmd, pcs, f->upload.address + UPLOAD_VIEWS_OFFSET, f->upload.address + CHUNKS_OFFSET, &v->cover,
                       trees, cover);
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, f->queries, 1);

    // Scene pass: colour + motion + depth into the frame targets (sampled by the previous frame's TAA pass).
    const VkPipelineStageFlags2 FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags2 READ = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, WRITE = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    const VkImageLayout SAMPLED = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ATTACHMENT = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (gallery && !vk->frozen_valid) {
        // Gallery background: the last world frame (its TAA output = history read this frame), else dark.
        const VkPipelineStageFlags2 COPY = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        vk_image_barrier(cmd, vk->frozen.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, FS, 0, VK_IMAGE_LAYOUT_UNDEFINED, COPY,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (v->last_frame_world) {
            VkImage src = vk->history[history_read].image;
            vk_image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, FS, 0, SAMPLED, COPY, VK_ACCESS_2_TRANSFER_READ_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageCopy region = {};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {vk->extent.width, vk->extent.height, 1};
            vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk->frozen.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vk_image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COPY, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, FS, READ, SAMPLED);
        } else {
            VkClearColorValue dark = {{0.08f, 0.08f, 0.09f, 1.0f}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, vk->frozen.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dark, 1, &range);
        }
        vk_image_barrier(cmd, vk->frozen.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, FS, READ, SAMPLED);
        vk->frozen_valid = true;
    }
    vk_image_barrier(cmd, vk->scene.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, FS, 0, SAMPLED, COLOR_OUT, WRITE, ATTACHMENT);
    vk_image_barrier(cmd, vk->motion.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, FS, 0, SAMPLED, COLOR_OUT, WRITE, ATTACHMENT);
    vk_image_barrier(cmd, vk->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, FS, 0, SAMPLED,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    color[0].imageView = vk->scene.view;
    color[1].imageView = vk->motion.view;
    for (u32 i = 0; i < 2; i++) {
        color[i].imageLayout = ATTACHMENT;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // sky covers every pixel the terrain does not
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = vk->depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;     // TAA reads it
    depth.clearValue.depthStencil.depth = 0.0f;       // reversed Z
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (f32)vk->extent.width, (f32)vk->extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, vk->extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(pcs), &pcs);
    if (gallery) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gal->frozen);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        u32 var = gallery_variant(gal->mode);
        for (u32 k = 0; k < gal->cells; k++) {
            GalleryElement el;
            gallery_element(gal, &v->cover, vg, (u32)gal->row * gal->cols + k, &el);
            gallery_cell(gal, &el, k, fc, (u8*)f->upload.mapped, jx_px, jy_px, v->time);
            VkDeviceAddress slot = f->upload.address + UPLOAD_NODES_OFFSET + (VkDeviceSize)k * GALLERY_CELL_STRIDE;
            const FrameConstants* cf = (const FrameConstants*)((u8*)f->upload.mapped + UPLOAD_NODES_OFFSET + (size_t)k * GALLERY_CELL_STRIDE);
            VkViewport cvp = {cf->viewport.x, cf->viewport.y, cf->viewport.z, cf->viewport.w, 0, 1};
            VkRect2D csc = {{(i32)cf->viewport.x, (i32)cf->viewport.y}, {(u32)cf->viewport.z, (u32)cf->viewport.w}};
            vkCmdSetViewport(cmd, 0, 1, &cvp);
            vkCmdSetScissor(cmd, 0, 1, &csc);
            PushConstants cp = pcs;
            cp.frame = slot;
            cp.view = slot + 1024;
            cp.chunks = slot + 1536;
            cp.veg = gal->tree_scene.address;
            vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(cp), &cp);
            if (gal->mode != 8) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gal->stage);
                vkCmdDraw(cmd, 6, 1, 0, 0);
            }
            VkPipeline p = el.tree ? gal->trees[var] : gal->rep == 4 ? gal->imp[var] : gal->gc[var];
            if (p) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                vkCmdDrawMeshTasksEXT(cmd, 1, 1, 1);
            }
        }
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(pcs), &pcs);
    }
    if (sel.count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->wireframe ? t->pipeline_wire : t->pipeline);
        vkCmdBindIndexBuffer(cmd, t->indices.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, t->index_count, sel.count, 0, 0, 0);
    }
    // Per-pass timestamps inside the render pass: approximate (passes overlap in the pipeline), good for trends.
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, f->queries, 2);
    if (trees && vg->chunk_count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vg->pipe_trees);
        vkCmdDrawMeshTasksEXT(cmd, vg->chunk_count, 1, 1);
    }
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, f->queries, 3);
    if (cover) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->cover_wire ? v->cover.wire : v->cover.pipeline);
        vkCmdDrawMeshTasksEXT(cmd, v->cover.cells, v->cover.cells, GC_LAYERS);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->cover_wire ? v->cover.wire_imp : v->cover.pipe_imp);
        vkCmdDrawMeshTasksEXT(cmd, v->cover.imp_cells, v->cover.imp_cells, 1);
    }
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, f->queries, 4);
    if (!gallery) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t->pipeline_sky);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);

    // TAA resolve: scene + history -> swapchain image + new history (with TAA off it copies the scene).
    vk_image_barrier(cmd, vk->scene.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COLOR_OUT, WRITE, ATTACHMENT, FS, READ, SAMPLED);
    vk_image_barrier(cmd, vk->motion.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COLOR_OUT, WRITE, ATTACHMENT, FS, READ, SAMPLED);
    vk_image_barrier(cmd, vk->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, FS, READ, SAMPLED);
    vk_image_barrier(cmd, vk->history[history_write].image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, FS, 0, SAMPLED, COLOR_OUT, WRITE, ATTACHMENT);
    vk_image_barrier(cmd, vk->images[image], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COLOR_OUT, 0, VK_IMAGE_LAYOUT_UNDEFINED, COLOR_OUT, WRITE, ATTACHMENT);
    color[0].imageView = vk->views[image];
    color[1].imageView = vk->history[history_write].view;
    ri.pDepthAttachment = NULL;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipe_taa);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    vk_image_barrier(cmd, vk->history[history_write].image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COLOR_OUT, WRITE, ATTACHMENT, FS, READ, SAMPLED);
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, f->queries, 5);

    if (overlay_boxes) {
        VkRenderingAttachmentInfo display = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        display.imageView = vk->views[image];
        display.imageLayout = ATTACHMENT;
        display.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        display.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &display;
        vk_image_barrier(cmd, vk->images[image], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COLOR_OUT, WRITE, ATTACHMENT, COLOR_OUT,
                         WRITE | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, ATTACHMENT);
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->overlay.pipeline);
        for (u32 b = 0; b < overlay_boxes; b++) {
            PushConstants op = pcs;
            op.overlay = f->upload.address + OVERLAY_OFFSET + (VkDeviceSize)b * sizeof(OverlayData);
            vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(op), &op);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
    }
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->queries, GPU_TIMESTAMPS - 1);

    VkBuf readback = {};
    bool shot = v->screenshot_path && vk->swap_transfer_src;
    if (shot) {
        readback = vk_buffer(vk, (VkDeviceSize)vk->extent.width * vk->extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        vk_image_barrier(cmd, vk->images[image], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {vk->extent.width, vk->extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, vk->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        vk_image_barrier(cmd, vk->images[image], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        vk_image_barrier(cmd, vk->images[image], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f->acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = vk->rendered[image];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(vk->queue, 1, &si, f->fence));
    f->submitted = true;
    vk->frame_index++;
    vk->history_valid = true;
    v->taa_frame++;
    v->prev_valid = true;
    memcpy(v->prev_pos, c->pos, sizeof(v->prev_pos));
    v->prev_right = right;
    v->prev_up = up;
    v->prev_fwd = fwd;
    v->prev_px = proj_x;
    v->prev_py = proj_y;
    v->prev_time = v->time;
    v->last_frame_world = !gallery;
    gal->prev_angle = gal->angle;

    if (shot) {
        VK_CHECK(vkWaitForFences(vk->device, 1, &f->fence, VK_TRUE, UINT64_MAX));
        char path[512];
        if (!strcmp(v->screenshot_path, "shot")) snprintf(path, sizeof(path), "shot_%04u.ppm", v->screenshot_counter++);
        else snprintf(path, sizeof(path), "%s", v->screenshot_path);
        write_ppm(path, (const u8*)readback.mapped, vk->extent.width, vk->extent.height,
                  vk->swap_format == VK_FORMAT_B8G8R8A8_SRGB || vk->swap_format == VK_FORMAT_B8G8R8A8_UNORM);
        vk_buffer_destroy(vk, &readback);
    } else if (v->screenshot_path) {
        fprintf(stderr, "screenshot: swapchain does not support TRANSFER_SRC\n");
    }
    v->screenshot_path = NULL;

    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk->rendered[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk->swapchain;
    pi.pImageIndices = &image;
    r = vkQueuePresentKHR(vk->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return false;
    if (r != VK_SUCCESS) FATAL("vkQueuePresentKHR = %d", (int)r);
    return true;
}
