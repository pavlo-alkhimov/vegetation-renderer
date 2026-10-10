// Overlay, top right: current FPS, FPS graph of the last 5 s, key list. The CPU fills an OverlayData block in the
// per-frame upload buffer; one blended quad draws it (shaders/overlay.slang).

#define OVERLAY_OFFSET  ((CHUNKS_OFFSET + MAX_TREE_CHUNKS * sizeof(TreeChunk) + 255) & ~(size_t)255)
#define OVERLAY_HISTORY 8192        // frame times; covers 5 s down to 0.6 ms frames
#define OVERLAY_SECONDS 5.0f

static_assert(OVERLAY_OFFSET + sizeof(OverlayData) <= UPLOAD_BYTES, "upload buffer too small");

typedef struct {
    f32 dt[OVERLAY_HISTORY];        // ring of frame times (s); newest at head - 1
    u32 head, count;
    f32 fps;                        // shown value, averaged over the last 0.5 s
    f32 fps_timer;                  // s since the shown value was updated
    u32 font[OVERLAY_GLYPHS * 2];
    VkPipeline pipeline;
    bool visible;
} Overlay;

// 5 x 7 glyphs, rows top to bottom. The shader draws lowercase letters as uppercase; other missing glyphs are blank.
static const struct { char c; const char* rows; } OVERLAY_FONT[] = {
    {'0', ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###."},
    {'1', "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###."},
    {'2', ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####"},
    {'3', "#####" "...#." "..#.." "...#." "....#" "#...#" ".###."},
    {'4', "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#."},
    {'5', "#####" "#...." "####." "....#" "....#" "#...#" ".###."},
    {'6', "..##." ".#..." "#...." "####." "#...#" "#...#" ".###."},
    {'7', "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#..."},
    {'8', ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###."},
    {'9', ".###." "#...#" "#...#" ".####" "....#" "...#." ".##.."},
    {'A', ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
    {'B', "####." "#...#" "#...#" "####." "#...#" "#...#" "####."},
    {'C', ".###." "#...#" "#...." "#...." "#...." "#...#" ".###."},
    {'D', "###.." "#..#." "#...#" "#...#" "#...#" "#..#." "###.."},
    {'E', "#####" "#...." "#...." "####." "#...." "#...." "#####"},
    {'F', "#####" "#...." "#...." "####." "#...." "#...." "#...."},
    {'G', ".###." "#...#" "#...." "#.###" "#...#" "#...#" ".####"},
    {'H', "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
    {'I', ".###." "..#.." "..#.." "..#.." "..#.." "..#.." ".###."},
    {'J', "..###" "...#." "...#." "...#." "...#." "#..#." ".##.."},
    {'K', "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#"},
    {'L', "#...." "#...." "#...." "#...." "#...." "#...." "#####"},
    {'M', "#...#" "##.##" "#.#.#" "#.#.#" "#...#" "#...#" "#...#"},
    {'N', "#...#" "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#"},
    {'O', ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
    {'P', "####." "#...#" "#...#" "####." "#...." "#...." "#...."},
    {'Q', ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#"},
    {'R', "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#"},
    {'S', ".####" "#...." "#...." ".###." "....#" "....#" "####."},
    {'T', "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#.."},
    {'U', "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
    {'V', "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#.."},
    {'W', "#...#" "#...#" "#...#" "#.#.#" "#.#.#" "#.#.#" ".#.#."},
    {'X', "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#"},
    {'Y', "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." "..#.."},
    {'Z', "#####" "....#" "...#." "..#.." ".#..." "#...." "#####"},
    {'.', "....." "....." "....." "....." "....." ".##.." ".##.."},
    {',', "....." "....." "....." "....." ".##.." "..#.." ".#..."},
    {':', "....." ".##.." ".##.." "....." ".##.." ".##.." "....."},
    {'-', "....." "....." "....." "#####" "....." "....." "....."},
    {'+', "....." "..#.." "..#.." "#####" "..#.." "..#.." "....."},
    {'=', "....." "....." "#####" "....." "#####" "....." "....."},
    {'/', "....." "....#" "...#." "..#.." ".#..." "#...." "....."},
    {'(', "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#."},
    {')', ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#..."},
    {'[', ".###." ".#..." ".#..." ".#..." ".#..." ".#..." ".###."},
    {']', ".###." "...#." "...#." "...#." "...#." "...#." ".###."},
    {'%', "##..#" "##..#" "...#." "..#.." ".#..." "#..##" "#..##"},
    {'?', ".###." "#...#" "....#" "...#." "..#.." "....." "..#.."},
};

static void overlay_init(Overlay* o, Vk* vk, const char* shader_dir)
{
    for (u32 i = 0; i < ARRAY_COUNT(OVERLAY_FONT); i++) {
        u32 g = (u32)(OVERLAY_FONT[i].c - 32);
        ASSERT(g < OVERLAY_GLYPHS && strlen(OVERLAY_FONT[i].rows) == 35);
        for (u32 b = 0; b < 35; b++)
            if (OVERLAY_FONT[i].rows[b] == '#') o->font[g * 2 + (b >> 5)] |= 1u << (b & 31);
    }
    char path[1024];
    snprintf(path, sizeof(path), "%soverlay.spv", shader_dir);
    VkShaderModule m = vk_load_shader(vk, path);
    PipelineDesc d = {m, "vs_overlay", "fs_overlay", VK_COMPARE_OP_ALWAYS, false, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL};
    d.blend = true;
    d.color_count = 1;
    d.color_formats[0] = vk->swap_format;      // drawn onto the swapchain image after TAA
    o->pipeline = vk_create_pipeline(vk, &d);
    vkDestroyShaderModule(vk->device, m, NULL);
    o->visible = true;
}

// Call once per frame with the real (unclamped) frame time.
static void overlay_frame(Overlay* o, f32 dt)
{
    o->dt[o->head] = dt;
    o->head = (o->head + 1) % OVERLAY_HISTORY;
    o->count = MIN(o->count + 1, (u32)OVERLAY_HISTORY);
    // Shown FPS: frames / time over the last 0.5 s, refreshed 4x per second so the digits stay readable.
    o->fps_timer += dt;
    if (o->fps_timer >= 0.25f || o->fps == 0) {
        o->fps_timer = 0;
        f32 t = 0;
        u32 n = 0;
        for (; n < o->count && t < 0.5f; n++) t += o->dt[(o->head + OVERLAY_HISTORY - 1 - n) % OVERLAY_HISTORY];
        o->fps = t > 0 ? n / t : 0;
    }
}

// Layout, graph and font. Text lines are written afterwards with overlay_line; lines 2-9 are covered by the graph.
static void overlay_begin(const Overlay* o, OverlayData* d, u32 width, u32 height)
{
    memset(d, 0, sizeof(*d));
    f32 s = (f32)MAX(1u, (height + 360) / 720);           // font texel size: 2 px at 1080p
    f32 cw = 6 * s, ch = 10 * s, pad = 6 * s;
    f32 w = OVERLAY_TEXT_COLS * cw + 2 * pad, h = 26 * ch + 2 * pad - 3 * s;
    d->screen = {(f32)width, (f32)height, 0, 0};
    d->panel = {(f32)width - w - 10, 10, w, h};
    d->text = {d->panel.x + pad, d->panel.y + pad, cw, ch};
    d->graph = {d->text.x, d->text.y + 2 * ch + 2 * s, OVERLAY_TEXT_COLS * cw - s, 8 * ch - 5 * s};
    memcpy(d->font, o->font, sizeof(d->font));

    // Graph: per column the frame rate over a 0.25 s window ending at the column's time, i.e. frames / time with
    // frames counted fractionally by overlap. Single frame times are useless under vsync: the loop blocks in bursts
    // (a 33 ms frame followed by a 0.1 ms one); a hitch still shows as a dip.
    const f32 bin = OVERLAY_SECONDS / OVERLAY_GRAPH_COLS, window = 0.25f;
    f32 frames[OVERLAY_GRAPH_COLS] = {}, covered[OVERLAY_GRAPH_COLS] = {};
    f32 back = 0;                                        // s before now at the frame's end
    for (u32 n = 0; n < o->count && back < OVERLAY_SECONDS + window; n++) {
        f32 dt = o->dt[(o->head + OVERLAY_HISTORY - 1 - n) % OVERLAY_HISTORY];
        if (dt <= 0) continue;
        // Column c (0 = newest) covers [c * bin, c * bin + window) before now.
        i32 c0 = MAX((i32)floorf((back - window) / bin) + 1, 0);
        i32 c1 = MIN((i32)floorf((back + dt) / bin), (i32)OVERLAY_GRAPH_COLS - 1);
        for (i32 c = c0; c <= c1; c++) {
            f32 overlap = MIN(back + dt, c * bin + window) - MAX(back, c * bin);
            if (overlap <= 0) continue;
            frames[c] += overlap / dt;
            covered[c] += overlap;
        }
        back += dt;
    }
    f32 fps_max = 0;
    for (u32 c = 0; c < OVERLAY_GRAPH_COLS; c++) {
        f32 fps = covered[c] > window * 0.5f ? frames[c] / covered[c] : 0;
        d->fps[OVERLAY_GRAPH_COLS - 1 - c] = fps;
        fps_max = MAX(fps_max, fps);
    }
    f32 scale = 75;                                      // 75, 150, 300, ...: the 60 FPS line stays inside
    while (scale < fps_max * 1.05f && scale < 7680) scale *= 2;
    d->params = {s, scale, 30, 60};
}

// One text line: key (highlighted, padded to 5 columns, may be NULL) followed by printf text.
static void overlay_line(OverlayData* d, u32 line, const char* key, const char* fmt, ...)
{
    char buf[OVERLAY_TEXT_COLS + 1];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    u8* row = (u8*)d->chars + line * OVERLAY_TEXT_COLS;
    u32 col = 0;
    if (key) {
        for (const char* k = key; *k && col < OVERLAY_TEXT_COLS; k++) row[col++] = (u8)*k | 0x80;
        col = MAX(col, 5u);
    }
    for (const char* t = buf; *t && col < OVERLAY_TEXT_COLS; t++) row[col++] = (u8)*t;
}
