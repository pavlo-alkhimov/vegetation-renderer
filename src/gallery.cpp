// Plant gallery (F2): every plant asset on its own gray stage in a grid of square cells, the camera orbiting each stage
// at 45 degrees elevation (all in sync). Elements: the ground-cover variants, then the tree types. View modes 1-8, the
// representation (mesh LODs, stripe, impostor; tree LODs) with R, rows with PgUp/PgDn, scrolling with the arrows,
// Home/End and the wheel. Each cell is drawn with the world's own pipelines in their single-element mode
// (FrameConstants.gc_bake.w < 0, gallery = 1); the world itself is not rendered, the last world frame stays as a
// dimmed background (TEX_FROZEN).

#define GALLERY_MAX_CELLS 64
#define GALLERY_FOV       (40.0f * 3.14159265f / 180.0f)
#define GALLERY_ELEVATION (45.0f * 3.14159265f / 180.0f)
#define GALLERY_SPIN      0.5f      // rad/s
#define GALLERY_CELL_STRIDE 2048    // bytes per cell in the upload buffer's node region: FrameConstants, view, chunk

enum { GV_FILL, GV_WIRE, GV_WIRE_CULL, GV_OVERDRAW, GV_COUNT };

static const char* GALLERY_MODE_NAMES[9] = {"", "FINAL", "TEXTURES", "WIREFRAME", "WIRE+CULL", "NORMALS", "ALPHA", "UV", "OVERDRAW"};
static const u32 GALLERY_MODE_DEBUG[9] = {DEBUG_SHADED, DEBUG_SHADED, DEBUG_UNLIT, DEBUG_SHADED, DEBUG_SHADED, DEBUG_NORMALS,
                                          DEBUG_ALPHA, DEBUG_UV, DEBUG_OVERDRAW};
static const char* GALLERY_REP_NAMES[5] = {"LOD0", "LOD1", "LOD2", "STRIPE", "IMPOSTOR"};

typedef struct {
    bool enabled, active;
    u32 rows;
    i32 row;                        // first visible row
    f32 angle, prev_angle;          // orbit azimuth (rad)
    bool paused;
    u32 mode;                       // 1..8
    u32 rep;                        // 0-2 mesh LOD, 3 stripe, 4 impostor (trees: LOD min(rep, 3))
    bool overlay_was_visible;
    VkPipeline stage, frozen;
    VkPipeline gc[GV_COUNT], imp[GV_COUNT], trees[GV_COUNT];
    VkBuf tree_instances, tree_scene;
    u32 gc_count, tree_count;
    // Layout of the current frame
    u32 cols, cells, cell_px, x0, y0;
} Gallery;

static u32 gallery_count(const Gallery* g) { return g->gc_count + g->tree_count; }

static u32 gallery_variant(u32 mode) { return mode == 3 ? GV_WIRE : mode == 4 ? GV_WIRE_CULL : mode == 8 ? GV_OVERDRAW : GV_FILL; }

static void gallery_init(Gallery* g, Vk* vk, const GroundCover* gc, const Vegetation* veg, const char* shader_dir)
{
    g->rows = 3;
    g->mode = 1;
    if (!vk->mesh_shaders) return;
    char path[1024];
    snprintf(path, sizeof(path), "%sgallery.spv", shader_dir);
    VkShaderModule m = vk_load_shader(vk, path);
    PipelineDesc stage = {m, "vs_stage", "fs_stage", VK_COMPARE_OP_GREATER, true, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL};
    g->stage = vk_create_pipeline(vk, &stage);
    PipelineDesc frozen = {m, "vs_fullscreen", "fs_frozen", VK_COMPARE_OP_ALWAYS, false, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL};
    g->frozen = vk_create_pipeline(vk, &frozen);
    vkDestroyShaderModule(vk->device, m, NULL);

    VkPolygonMode line = vk->wireframe_supported ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    struct { VkPipeline* out; const char* name; const char* fs; const char* wire_fs; VkPipeline fill; } sets[3] = {
        {g->gc, "gc", "fs_gc", "fs_gc_wire", gc->pipeline},
        {g->imp, "gc_imp", "fs_gc_imp", "fs_gc_imp_wire", gc->pipe_imp},
        {g->trees, "trees", "fs_trees", "fs_trees_wire", veg->pipe_trees},
    };
    for (u32 i = 0; i < 3; i++) {
        if (!sets[i].fill) continue;
        sets[i].out[GV_FILL] = sets[i].fill;
        sets[i].out[GV_WIRE] = veg_pipeline_ex(vk, shader_dir, sets[i].name, sets[i].wire_fs, line, VK_CULL_MODE_NONE, false);
        sets[i].out[GV_WIRE_CULL] = veg_pipeline_ex(vk, shader_dir, sets[i].name, sets[i].wire_fs, line, VK_CULL_MODE_BACK_BIT, false);
        sets[i].out[GV_OVERDRAW] = veg_pipeline_ex(vk, shader_dir, sets[i].name, sets[i].fs, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, true);
    }
    g->gc_count = gc->enabled ? MIN(gc->variant_count, (u32)GC_MAX_VARIANTS) : 0;
    if (veg->enabled) {
        // One instance per tree type at the origin (scale byte 128 = 1.0, yaw 0, mid tint); one chunk per cell selects it.
        TreeInstance inst[TREE_TYPES];
        for (u32 t = 0; t < TREE_TYPES; t++) inst[t] = {0, 0, 0, t | 0u << 8 | 128u << 16 | 128u << 24};
        g->tree_instances = vk_buffer_static(vk, sizeof(inst), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, inst);
        VegScene scene = {g->tree_instances.address, veg->type_buf.address, veg->meshlet_buf.address, veg->vertex_buf.address,
                          veg->triangle_buf.address};
        g->tree_scene = vk_buffer_static(vk, sizeof(scene), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scene);
        g->tree_count = TREE_TYPES;
    }
    g->enabled = gallery_count(g) > 0;
}

// Square cells over the screen above the key bar; clamps the scroll position.
static void gallery_layout(Gallery* g, u32 width, u32 height, f32 bar_px)
{
    u32 avail = (u32)MAX((f32)height - bar_px, 64.0f);
    g->cell_px = MAX(avail / g->rows, 32u);
    g->cols = MAX(width / g->cell_px, 1u);
    if (g->cols * g->rows > GALLERY_MAX_CELLS) g->cols = MAX(GALLERY_MAX_CELLS / g->rows, 1u);
    g->x0 = (width - g->cols * g->cell_px) / 2;
    g->y0 = (avail - g->rows * g->cell_px) / 2;
    i32 total_rows = (i32)((gallery_count(g) + g->cols - 1) / g->cols);
    g->row = CLAMP(g->row, 0, MAX(total_rows - (i32)g->rows, 0));
    g->cells = MIN(g->rows * g->cols, gallery_count(g) - MIN((u32)g->row * g->cols, gallery_count(g)));
}

// Element shown in cell k: ground-cover variant or tree type, its framing and its label.
typedef struct {
    bool tree;
    u32 index;                      // variant or tree type
    v3 centre;                      // orbit target
    f32 radius;                     // bounding sphere
    f32 stage;                      // stage half size
    char name[64];
    char rep[48];
} GalleryElement;

static void gallery_element(const Gallery* g, const GroundCover* gc, const Vegetation* veg, u32 e, GalleryElement* el)
{
    memset(el, 0, sizeof(*el));
    if (e < g->gc_count) {
        el->index = e;
        el->centre = v3_make(0, gc->heights[e] * 0.5f, 0);
        el->radius = MAX(gc->bounds[e], 0.03f);
        el->stage = MAX(gc->radii[e] * 1.25f, 0.12f);
        snprintf(el->name, sizeof(el->name), "%s", gc->names[e]);
        char* lod = strstr(el->name, "_LOD0");
        if (lod) *lod = 0;
        u32 r = g->rep;
        if (r < 3) snprintf(el->rep, sizeof(el->rep), "%s %u TRI %u MSH", GALLERY_REP_NAMES[r], gc->tris[e][r], gc->meshlets[e][r]);
        else snprintf(el->rep, sizeof(el->rep), "%s %u TRI", GALLERY_REP_NAMES[r], r == 3 ? 32u : 2u);
    } else {
        u32 t = e - g->gc_count;
        const TreeType* tt = &veg->types[t];
        el->tree = true;
        el->index = t;
        el->centre = v3_make(tt->bounds.x, tt->bounds.y, tt->bounds.z);
        el->radius = tt->bounds.w;
        el->stage = MAX(tt->shape.z * tt->bark_color.w * 1.2f, 2.0f);
        snprintf(el->name, sizeof(el->name), "TREE %s %u", SPECIES[t / TREE_VARIANTS].name, t % TREE_VARIANTS + 1);
        u32 lod = MIN(g->rep, 3u);
        snprintf(el->rep, sizeof(el->rep), "LOD%u %u TRI %u MSH", lod, veg->lod_tris[t][lod], tt->meshlet_count[lod]);
    }
    for (char* c = el->name; *c; c++) if (*c == '_') *c = ' ';
}

// Constants of cell k (slot in the node region): FrameConstants from the world's, an orbit view, a one-instance tree
// chunk. jitter_px: sub-pixel TAA jitter in pixels (0 for modes without history).
static void gallery_cell(const Gallery* g, const GalleryElement* el, u32 k, const FrameConstants* base, u8* mapped, f32 jx_px,
                         f32 jy_px, f32 time)
{
    u8* slot = mapped + UPLOAD_NODES_OFFSET + (size_t)k * GALLERY_CELL_STRIDE;
    FrameConstants* fc = (FrameConstants*)slot;
    ViewConstants* view = (ViewConstants*)(slot + 1024);
    TreeChunk* chunk = (TreeChunk*)(slot + 1536);
    static_assert(sizeof(FrameConstants) <= 1024 && sizeof(ViewConstants) <= 512, "gallery cell layout");
    static_assert(UPLOAD_NODES_OFFSET + GALLERY_MAX_CELLS * GALLERY_CELL_STRIDE <= UPLOAD_BYTES, "gallery cells");

    f32 dist = el->radius / sinf(GALLERY_FOV * 0.5f) * 1.1f, near = dist * 0.05f;
    f32 py = 1.0f / tanf(GALLERY_FOV * 0.5f), px = py;
    ViewConstants prev;
    for (u32 pass = 0; pass < 2; pass++) {
        f32 az = pass == 0 ? g->angle : g->prev_angle;
        v3 cam = v3_add(el->centre, v3_scale(v3_make(cosf(GALLERY_ELEVATION) * sinf(az), sinf(GALLERY_ELEVATION),
                                                     cosf(GALLERY_ELEVATION) * cosf(az)), dist));
        v3 fwd = v3_norm(v3_sub(el->centre, cam));
        v3 right = v3_norm(v3_cross(v3_make(0, 1, 0), fwd));
        v3 up = v3_cross(fwd, right);
        if (pass == 0) {
            view_perspective(view, cam, right, up, fwd, px, py, near, jx_px * 2.0f / g->cell_px, jy_px * 2.0f / g->cell_px);
            *fc = *base;
            fc->cam_pos = {cam.x, cam.y, cam.z, near};
            fc->cam_right = {right.x, right.y, right.z, px};
            fc->cam_up = {up.x, up.y, up.z, py};
            fc->cam_fwd = {fwd.x, fwd.y, fwd.z, time};
        } else {
            view_perspective(&prev, cam, right, up, fwd, px, py, near, 0, 0);
        }
    }
    memcpy(view->prev_view_proj, prev.view_proj, sizeof(prev.view_proj));
    fc->terrain = {0, 0, 1, 0};
    fc->sun_dir.w = 0;                                       // no fog
    fc->veg = {1e9f, 1, g->cell_px * 0.5f * py, el->stage};
    fc->wind = {1, 0, 0, 0};                                 // still
    fc->taa.x = time;
    fc->gc_bake = {0, 0, 0, -1};
    fc->gc_bake_pass = el->tree ? 0 : el->index;
    fc->gallery = 1;
    fc->gallery_rep = el->tree ? MIN(g->rep, 3u) : g->rep;
    for (u32 l = 0; l < GC_LAYERS; l++) fc->gc_cells[l] = 0;
    fc->gc_imp_cells = 0;
    fc->shadows = 0;
    fc->debug_mode = GALLERY_MODE_DEBUG[g->mode];
    u32 cx = k % g->cols, cy = k / g->cols;
    fc->viewport = {(f32)(g->x0 + cx * g->cell_px), (f32)(g->y0 + cy * g->cell_px), (f32)g->cell_px, (f32)g->cell_px};
    *chunk = {el->index, 1, 0, 0};
}
