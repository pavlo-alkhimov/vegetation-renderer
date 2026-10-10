// cook_terrain: GeoTIFF elevation tiles -> one cooked heightfield (.vrh, see terrain_file.h).
//
//   cook_terrain [-o out.vrh] [--step N] <tile.tif | directory>...
//   cook_terrain --info <tile.tif>      print all TIFF tags (diagnostics)
//
// Tiles are placed on a common grid by their georeferencing (all tiles must share CRS and pixel size). Samples
// without data (missing tiles, nodata) are filled by pull-push interpolation and counted in the header.
// --step N averages N x N samples (e.g. 2 -> 2 m grid) to fit smaller GPUs or for quick tests.
#include "../base.h"
#include "../terrain_file.h"
#include <atomic>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "inflate.cpp"
#include "tiff.cpp"

typedef struct {
    char path[1024];
    u32 width, height, epsg;
    f64 x0, y0, sx, sy;
} TileInfo;

static TileInfo* g_tiles;
static u32 g_tile_count, g_tile_cap;

static bool ends_with_tif(const char* s)
{
    size_t n = strlen(s);
    return (n > 4 && (!strcmp(s + n - 4, ".tif") || !strcmp(s + n - 4, ".TIF"))) ||
           (n > 5 && (!strcmp(s + n - 5, ".tiff") || !strcmp(s + n - 5, ".TIFF")));
}

static void add_path(const char* path)
{
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE d = (GetFileAttributesA(path) & FILE_ATTRIBUTE_DIRECTORY) ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    if (d != INVALID_HANDLE_VALUE) {
        do {
            if (!ends_with_tif(fd.cFileName)) continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", path, fd.cFileName);
            add_path(full);
        } while (FindNextFileA(d, &fd));
        FindClose(d);
        return;
    }
#else
    DIR* d = opendir(path);
    if (d) {
        for (struct dirent* e; (e = readdir(d));) {
            if (!ends_with_tif(e->d_name)) continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            add_path(full);
        }
        closedir(d);
        return;
    }
#endif
    if (g_tile_count == g_tile_cap) {
        g_tile_cap = g_tile_cap ? g_tile_cap * 2 : 256;
        g_tiles = (TileInfo*)realloc(g_tiles, g_tile_cap * sizeof(TileInfo));
    }
    TileInfo* t = &g_tiles[g_tile_count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", path);
}

// Fills NaN samples from coarser averages (pull-push). Returns the number of filled samples.
static u64 fill_holes(f32* h, u32 w, u32 hgt)
{
    u64 holes = 0;
    for (u64 i = 0; i < (u64)w * hgt; i++) holes += isnan(h[i]);
    if (!holes) return 0;
    if (holes == (u64)w * hgt) FATAL("no valid samples");
    // Pull: build the pyramid, averaging valid children.
    f32* levels[32];
    u32 lw[32], lh[32], n = 0;
    levels[0] = h; lw[0] = w; lh[0] = hgt;
    while (lw[n] > 1 || lh[n] > 1) {
        u32 cw = (lw[n] + 1) / 2, ch = (lh[n] + 1) / 2;
        f32* c = (f32*)malloc((size_t)cw * ch * sizeof(f32));
        for (u32 y = 0; y < ch; y++)
            for (u32 x = 0; x < cw; x++) {
                f32 sum = 0; int cnt = 0;
                for (u32 dy = 0; dy < 2; dy++)
                    for (u32 dx = 0; dx < 2; dx++) {
                        u32 sx = MIN(2 * x + dx, lw[n] - 1), sy = MIN(2 * y + dy, lh[n] - 1);
                        f32 v = levels[n][(size_t)sy * lw[n] + sx];
                        if (!isnan(v)) { sum += v; cnt++; }
                    }
                c[(size_t)y * cw + x] = cnt ? sum / cnt : NAN;
            }
        n++;
        levels[n] = c; lw[n] = cw; lh[n] = ch;
    }
    // Push: fill holes from the bilinearly interpolated coarser level.
    for (u32 l = n; l-- > 0;) {
        const f32* c = levels[l + 1];
        for (u32 y = 0; y < lh[l]; y++)
            for (u32 x = 0; x < lw[l]; x++) {
                f32* v = &levels[l][(size_t)y * lw[l] + x];
                if (!isnan(*v)) continue;
                f32 fx = CLAMP((x - 0.5f) * 0.5f, 0.0f, (f32)(lw[l + 1] - 1)), fy = CLAMP((y - 0.5f) * 0.5f, 0.0f, (f32)(lh[l + 1] - 1));
                u32 x0 = (u32)fx, y0 = (u32)fy, x1 = MIN(x0 + 1, lw[l + 1] - 1), y1 = MIN(y0 + 1, lh[l + 1] - 1);
                f32 ax = fx - x0, ay = fy - y0;
                f32 a = c[(size_t)y0 * lw[l + 1] + x0], b = c[(size_t)y0 * lw[l + 1] + x1];
                f32 d = c[(size_t)y1 * lw[l + 1] + x0], e = c[(size_t)y1 * lw[l + 1] + x1];
                *v = (a * (1 - ax) + b * ax) * (1 - ay) + (d * (1 - ax) + e * ax) * ay;
            }
        if (l + 1 <= n) free(levels[l + 1]);
    }
    return holes;
}

int main(int argc, char** argv)
{
    const char* out_path = "data/cooked/terrain.vrh";
    u32 step = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--step") && i + 1 < argc) step = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--info") && i + 1 < argc) {
            size_t size;
            u8* file = (u8*)read_file(argv[++i], &size);
            if (!file) FATAL("cannot read %s", argv[i]);
            tiff_dump(file, size);
            Tiff t;
            const char* err = tiff_open(&t, file, size);
            char desc[256];
            tiff_describe(&t, desc, sizeof(desc));
            printf("%s\n%s\n", desc, err ? err : "readable");
            return err ? 1 : 0;
        }
        else if (argv[i][0] == '-') FATAL("usage: cook_terrain [-o out.vrh] [--step N] <tile.tif | dir>...");
        else add_path(argv[i]);
    }
    if (!g_tile_count) FATAL("no input tiles (usage: cook_terrain [-o out.vrh] [--step N] <tile.tif | dir>...)");
    if (step < 1) step = 1;

    // Pass 1: headers -> common grid.
    f64 min_x = 1e300, min_y = 1e300, max_x = -1e300, max_y = -1e300, sx = 0, sy = 0;
    u32 epsg = 0;
    for (u32 i = 0; i < g_tile_count; i++) {
        TileInfo* ti = &g_tiles[i];
        size_t size;
        u8* file = (u8*)read_file(ti->path, &size);
        if (!file) FATAL("cannot read %s", ti->path);
        Tiff t;
        const char* err = tiff_open(&t, file, size);
        if (err) {
            char desc[256];
            tiff_describe(&t, desc, sizeof(desc));
            FATAL("%s: %s\n  layout: %s\n  all tags: cook_terrain --info %s", ti->path, err, desc, ti->path);
        }
        if (t.samples > 1 && i == 0) printf("note: %u samples per pixel (%u extra); using band 0 as elevation\n", t.samples, t.extra_samples);
        if (!t.has_geo) FATAL("%s: no georeferencing (ModelTiepoint/ModelPixelScale)", ti->path);
        if (!sx) { sx = t.sx; sy = t.sy; epsg = t.epsg; }
        if (fabs(t.sx - sx) > 1e-6 || fabs(t.sy - sy) > 1e-6) FATAL("%s: pixel size differs from first tile", ti->path);
        if (t.epsg != epsg) FATAL("%s: EPSG %u differs from first tile (%u)", ti->path, t.epsg, epsg);
        if (fabs(sx - sy) > 1e-6) FATAL("%s: non-square pixels not supported", ti->path);
        ti->width = t.width; ti->height = t.height; ti->epsg = t.epsg;
        ti->x0 = t.x0; ti->y0 = t.y0; ti->sx = t.sx; ti->sy = t.sy;
        min_x = MIN(min_x, t.x0); max_x = MAX(max_x, t.x0 + (t.width - 1) * t.sx);
        max_y = MAX(max_y, t.y0); min_y = MIN(min_y, t.y0 - (t.height - 1) * t.sy);
        free(file);
    }
    u32 w = (u32)llround((max_x - min_x) / sx) + 1, h = (u32)llround((max_y - min_y) / sy) + 1;
    printf("%u tiles, EPSG:%u, %.3f m, grid %u x %u, E %.1f..%.1f N %.1f..%.1f\n",
           g_tile_count, epsg, sx, w, h, min_x, max_x, min_y, max_y);
    if ((u64)w * h > (u64)1 << 31) FATAL("grid too large (%u x %u)", w, h);

    // Pass 2: decode tiles in parallel into the grid (row 0 = south).
    f32* grid = (f32*)malloc((size_t)w * h * sizeof(f32));
    for (size_t i = 0; i < (size_t)w * h; i++) grid[i] = NAN;
    std::atomic<u32> next{0};
    auto worker = [&]() {
        for (u32 i; (i = next.fetch_add(1)) < g_tile_count;) {
            TileInfo* ti = &g_tiles[i];
            size_t size;
            u8* file = (u8*)read_file(ti->path, &size);
            Tiff t;
            const char* err = file ? tiff_open(&t, file, size) : "cannot read";
            f32* px = (f32*)malloc((size_t)ti->width * ti->height * sizeof(f32));
            if (!err) err = tiff_read_f32(&t, px);
            if (err) FATAL("%s: %s", ti->path, err);
            i64 gx = llround((ti->x0 - min_x) / sx), gy = llround((ti->y0 - min_y) / sy);
            for (u32 r = 0; r < ti->height; r++) {
                f32* dst = grid + (size_t)(gy - r) * w + gx;
                const f32* src = px + (size_t)r * ti->width;
                for (u32 c = 0; c < ti->width; c++) if (!isnan(src[c])) dst[c] = src[c];
            }
            free(px);
            free(file);
        }
    };
    u32 nthreads = MAX(1u, MIN(16u, std::thread::hardware_concurrency()));
    std::thread threads[16];
    for (u32 i = 0; i < nthreads; i++) threads[i] = std::thread(worker);
    for (u32 i = 0; i < nthreads; i++) threads[i].join();

    u64 holes = fill_holes(grid, w, h);
    if (holes) printf("filled %llu samples without data (%.2f %%)\n", (unsigned long long)holes, 100.0 * holes / ((f64)w * h));

    if (step > 1) {
        u32 nw = w / step, nh = h / step;
        for (u32 y = 0; y < nh; y++)
            for (u32 x = 0; x < nw; x++) {
                f64 sum = 0;
                for (u32 dy = 0; dy < step; dy++)
                    for (u32 dx = 0; dx < step; dx++) sum += grid[(size_t)(y * step + dy) * w + x * step + dx];
                grid[(size_t)y * nw + x] = (f32)(sum / (step * step));
            }
        // Box centre of output sample (0,0) is (step - 1) / 2 input samples from input sample (0,0).
        min_x += (step - 1) * 0.5 * sx; min_y += (step - 1) * 0.5 * sy;
        w = nw; h = nh; sx *= step;
    }

    f32 hmin = 1e30f, hmax = -1e30f;
    for (size_t i = 0; i < (size_t)w * h; i++) { hmin = MIN(hmin, grid[i]); hmax = MAX(hmax, grid[i]); }
    TerrainFileHeader hdr = {};
    hdr.magic = TERRAIN_FILE_MAGIC; hdr.version = TERRAIN_FILE_VERSION;
    hdr.width = w; hdr.height = h; hdr.spacing = (f32)sx;
    hdr.height_min = hmin; hdr.height_scale = MAX(hmax - hmin, 1e-3f) / 65535.0f;
    hdr.epsg = epsg; hdr.origin_e = min_x; hdr.origin_n = min_y;
    hdr.holes_filled = (u32)MIN(holes, (u64)0xffffffffu);
    u16* q = (u16*)malloc((size_t)w * h * sizeof(u16));
    for (size_t i = 0; i < (size_t)w * h; i++) q[i] = (u16)lrintf(CLAMP((grid[i] - hmin) / hdr.height_scale, 0.0f, 65535.0f));

    char dir[1024];   // create the parent directories of out_path
    snprintf(dir, sizeof(dir), "%s", out_path);
    for (char* p = dir + 1; *p; p++)
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; make_dir(dir); *p = c; }
    FILE* f = fopen(out_path, "wb");
    if (!f) FATAL("cannot write %s (does the directory exist?)", out_path);
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(q, sizeof(u16), (size_t)w * h, f);
    if (fclose(f)) FATAL("write failed: %s", out_path);
    printf("%s: %u x %u, %.2f m, height %.2f..%.2f m (step %.4f m)\n", out_path, w, h, sx, hmin, hmax, hdr.height_scale);
    return 0;
}
