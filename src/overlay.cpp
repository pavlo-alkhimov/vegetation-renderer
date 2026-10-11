// Overlay: performance panel (FPS, GPU time, FPS graph with a logarithmic time axis), key bar along the bottom, gallery
// labels. The CPU fills OverlayData blocks in the per-frame upload buffer; one blended quad per box draws them
// (shaders/overlay.slang).

#define OVERLAY_OFFSET  ((CHUNKS_OFFSET + MAX_TREE_CHUNKS * sizeof(TreeChunk) + 255) & ~(size_t)255)
#define OVERLAY_SECONDS 60.0f       // history shown by the graph (logarithmic time axis)
#define GRAPH_A 0.05f               // s: time scale of the first columns
#define GRAPH_B 7.09f               // GRAPH_A * (exp(GRAPH_B) - 1) = OVERLAY_SECONDS

// Seconds before now where graph column c (0 = newest) starts; and the column of a given age.
static f32 overlay_graph_time(f32 c) { return GRAPH_A * (expf(GRAPH_B * c / OVERLAY_GRAPH_COLS) - 1.0f); }
static f32 overlay_graph_col(f32 t) { return logf(t / GRAPH_A + 1.0f) * OVERLAY_GRAPH_COLS / GRAPH_B; }

static_assert(OVERLAY_OFFSET + OVERLAY_MAX_BOXES * sizeof(OverlayData) <= UPLOAD_BYTES, "upload buffer too small");

typedef struct {
    // The graph's own columns are the only stored history: column c (0 = newest) holds the frames (fractional) and the
    // frame time that fall into its age range [overlay_graph_time(c), overlay_graph_time(c + 1)).
    f32 col_frames[OVERLAY_GRAPH_COLS], col_time[OVERLAY_GRAPH_COLS];
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
    // Age every column's content by dt: its range shifts to [t(c) + dt, t(c + 1) + dt) and is spread, assuming even
    // density, over the columns it now overlaps. Columns widen with age, so content merges (compresses) as it moves left;
    // beyond OVERLAY_SECONDS it drops out. The new frame then fills [0, dt).
    const u32 N = OVERLAY_GRAPH_COLS;
    static f32 t[OVERLAY_GRAPH_COLS + 1];
    if (t[N] == 0) for (u32 c = 0; c <= N; c++) t[c] = overlay_graph_time((f32)c);
    if (dt <= 0) return;
    f32 nf[OVERLAY_GRAPH_COLS] = {}, nt[OVERLAY_GRAPH_COLS] = {};
    u32 k0 = 0;
    for (u32 c = 0; c < N; c++) {
        if (o->col_time[c] <= 0) continue;
        f32 a = t[c] + dt, b = t[c + 1] + dt, w = t[c + 1] - t[c];
        while (k0 < N && t[k0 + 1] <= a) k0++;
        for (u32 k = k0; k < N && t[k] < b; k++) {
            f32 share = (MIN(b, t[k + 1]) - MAX(a, t[k])) / w;
            if (share <= 0) continue;
            nf[k] += o->col_frames[c] * share;
            nt[k] += o->col_time[c] * share;
        }
    }
    for (u32 k = 0; k < N && t[k] < dt; k++) {
        f32 part = MIN(dt, t[k + 1]) - t[k];
        nf[k] += part / dt;
        nt[k] += part;
    }
    memcpy(o->col_frames, nf, sizeof(nf));
    memcpy(o->col_time, nt, sizeof(nt));
    // Shown FPS: frames / time over the last 0.5 s, refreshed 4x per second so the digits stay readable.
    o->fps_timer += dt;
    if (o->fps_timer >= 0.25f || o->fps == 0) {
        o->fps_timer = 0;
        f32 fr = 0, tm = 0;
        for (u32 k = 0; k < N && t[k] < 0.5f; k++) { fr += o->col_frames[k]; tm += o->col_time[k]; }
        o->fps = tm > 0 ? fr / tm : 0;
    }
}
static f32 overlay_texel(u32 height) { return (f32)MAX(1u, (height + 360) / 720); }   // font texel: 2 px at 1080p

// Pixel height of a box with `lines` text lines.
// Time axis labels under the graph (logarithmic): ages at their columns, "NOW" at the right.
static void overlay_graph_legend(char* out, u32 cols)
{
    memset(out, ' ', cols);
    out[cols] = 0;
    static const struct { f32 t; const char* text; } MARKS[] = {{OVERLAY_SECONDS, "60S"}, {10.0f, "10S"}, {1.0f, "1S"}};
    for (u32 i = 0; i < ARRAY_COUNT(MARKS); i++) {
        f32 x = 1.0f - overlay_graph_col(MARKS[i].t) / OVERLAY_GRAPH_COLS;
        u32 len = (u32)strlen(MARKS[i].text);
        i32 at = CLAMP((i32)lrintf(x * cols) - (i32)len / 2, 0, (i32)(cols - len));
        memcpy(out + at, MARKS[i].text, len);
    }
    memcpy(out + cols - 3, "NOW", 3);
}

static f32 overlay_box_height(u32 height, u32 lines)
{
    f32 s = overlay_texel(height);
    return lines * 10 * s + 8 * s - 3 * s;
}

// Box with a cols x lines text grid, top-left at (x, y) px, no graph. Text is written with overlay_line.
static void overlay_box(const Overlay* o, OverlayData* d, u32 width, u32 height, f32 x, f32 y, u32 cols, u32 lines)
{
    memset(d, 0, sizeof(*d));
    f32 s = overlay_texel(height), cw = 6 * s, ch = 10 * s, pad = 4 * s;
    cols = MIN(cols, (u32)OVERLAY_TEXT_CHARS / MAX(lines, 1u));
    d->screen = {(f32)width, (f32)height, 0, 0};
    d->panel = {x, y, cols * cw + 2 * pad, overlay_box_height(height, lines)};
    d->text = {x + pad, y + pad, cw, ch};
    d->grid = {(f32)cols, (f32)lines, 0, 0};
    d->params = {s, 1, 0, 0};
    memcpy(d->font, o->font, sizeof(d->font));
}

// Performance panel, top right: line 0 FPS, line 1 GPU, lines 2-9 the FPS graph, lines 10-11 its legend.
static void overlay_begin(const Overlay* o, OverlayData* d, u32 width, u32 height)
{
    f32 s = overlay_texel(height), cw = 6 * s, ch = 10 * s;
    f32 w = OVERLAY_PANEL_COLS * cw + 8 * s;
    overlay_box(o, d, width, height, (f32)width - w - 10, 10, OVERLAY_PANEL_COLS, 12);
    d->graph = {d->text.x, d->text.y + 2 * ch + 2 * s, OVERLAY_PANEL_COLS * cw - s, 8 * ch - 5 * s};

    // Display: column c shows frames / time of its own content, widened towards older columns to at least 0.1 s (single
    // frames are useless under vsync, the loop blocks in bursts); a hitch still shows as a dip.
    const u32 N = OVERLAY_GRAPH_COLS;
    f32 fps_max = 0;
    for (u32 c = 0; c < N; c++) {
        f32 fr = 0, tm = 0, start = overlay_graph_time((f32)c);
        for (u32 k = c; k < N; k++) {
            fr += o->col_frames[k];
            tm += o->col_time[k];
            if (overlay_graph_time((f32)(k + 1)) - start >= 0.1f) break;
        }
        f32 width = MAX(overlay_graph_time((f32)(c + 1)) - start, 0.1f);
        f32 fps = tm > width * 0.5f ? fr / tm : 0;
        d->fps[N - 1 - c] = fps;
        fps_max = MAX(fps_max, fps);
    }
    f32 scale = 75;                                      // 75, 150, 300, ...: the 60 FPS line stays inside
    while (scale < fps_max * 1.05f && scale < 7680) scale *= 2;
    d->params = {s, scale, 30, 60};
}

// One text line: key (highlighted, padded to 5 columns, may be NULL) followed by printf text.
static void overlay_line(OverlayData* d, u32 line, const char* key, const char* fmt, ...)
{
    u32 cols = (u32)d->grid.x;
    if (line >= (u32)d->grid.y) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    u8* row = (u8*)d->chars + line * cols;
    u32 col = 0;
    if (key) {
        for (const char* k = key; *k && col < cols; k++) row[col++] = (u8)*k | 0x80;
        col = MAX(col, 5u);
    }
    for (const char* t = buf; *t && col < cols; t++) row[col++] = (u8)*t;
}

// Key bar at the bottom of the screen: "key text" entries on a grid of equal slots (entries line up in columns; a long
// one takes several slots), over as many lines as needed.
typedef struct {
    u32 cols, slot, line, col;
    u8 cells[OVERLAY_TEXT_CHARS];
    u8 attrs[OVERLAY_TEXT_CHARS];   // per cell: 0 or 1 + DEBUG_TINT index (text on that colour)
} KeyBar;

static void keybar_begin(KeyBar* b, u32 width, u32 height)
{
    memset(b, 0, sizeof(*b));
    f32 s = overlay_texel(height);
    b->cols = MIN((u32)((width - 12 - 8 * s) / (6 * s)), 400u);
    b->slot = b->cols / MAX(b->cols / 22u, 1u);                     // ~22 characters per slot, spread evenly
}

static void keybar_add_v(KeyBar* b, u8 attr, const char* key, const char* fmt, va_list args)
{
    char text[256];
    vsnprintf(text, sizeof(text), fmt, args);
    u32 n = (u32)(strlen(key) + 1 + strlen(text));
    u32 span = (n + 2 + b->slot - 1) / b->slot * b->slot;          // whole slots, at least 2 spaces after the text
    if (b->col > 0 && b->col + MIN(span, b->cols) > b->cols) { b->line++; b->col = 0; }
    u32 start = b->col;
    for (const char* k = key; *k; k++)
        if (b->col < b->cols && (b->line + 1) * b->cols <= OVERLAY_TEXT_CHARS) b->cells[b->line * b->cols + b->col++] = (u8)*k | 0x80;
    if (*key) b->col++;
    for (const char* t = text; *t; t++)
        if (b->col < b->cols && (b->line + 1) * b->cols <= OVERLAY_TEXT_CHARS) {
            b->attrs[b->line * b->cols + b->col] = attr;
            b->cells[b->line * b->cols + b->col++] = (u8)*t;
        }
    b->col = MIN(start + span, b->cols);
}

static void keybar_add(KeyBar* b, const char* key, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    keybar_add_v(b, 0, key, fmt, args);
    va_end(args);
}

// Entry whose text stands on a debug colour (tint = DEBUG_TINT index): legends of colour-coded views.
static void keybar_add_colored(KeyBar* b, u32 tint, const char* key, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    keybar_add_v(b, (u8)(tint + 1), key, fmt, args);
    va_end(args);
}

static u32 keybar_lines(const KeyBar* b) { return b->col > 0 ? b->line + 1 : b->line; }

// Writes the bar as a box along the bottom edge; returns its height in px.
static f32 keybar_end(const Overlay* o, const KeyBar* b, OverlayData* d, u32 width, u32 height)
{
    u32 lines = MAX(keybar_lines(b), 1u);
    f32 h = overlay_box_height(height, lines);
    overlay_box(o, d, width, height, 6, (f32)height - h - 6, b->cols, lines);
    memcpy(d->chars, b->cells, (size_t)lines * b->cols);
    memcpy(d->attrs, b->attrs, (size_t)lines * b->cols);
    return h + 12;
}