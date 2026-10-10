// Minimal GeoTIFF reader for elevation rasters.
// Supports: classic TIFF (II/MM), strips or tiles, compression none/LZW/Deflate, predictor 1/2/3,
// 8/16/32-bit integer or 32/64-bit float samples, ModelTiepoint + ModelPixelScale, RasterType, GDAL_NODATA.
// Multi-sample files (e.g. elevation + alpha/mask, chunky or planar): band 0 is read. Colour images are rejected.
// Not supported: BigTIFF, ModelTransformation with rotation.

typedef struct {
    u32 width, height;
    u32 bits, sample_format;        // sample_format: 1 uint, 2 int, 3 float
    u32 compression, predictor;
    u32 samples, planar, photometric, extra_samples;
    u32 tile_w, tile_h;             // tile size; for strips tile_w = width, tile_h = rows per strip
    u32 chunk_count;                // all chunks; band 0 uses the first chunks_per_band (planar) or all (chunky)
    u32 chunks_per_band;
    const u8* file; size_t file_size;
    bool big_endian;
    u32 chunk_offsets_pos, chunk_counts_pos, chunk_offsets_type, chunk_counts_type;
    // Georeferencing: centre of pixel (0,0) and pixel size; y grows north, rows grow south.
    f64 x0, y0, sx, sy;
    bool has_geo;
    u32 epsg;
    bool has_nodata;
    f64 nodata;
} Tiff;

static u16 tiff_u16(const Tiff* t, size_t pos)
{
    const u8* p = t->file + pos;
    return t->big_endian ? (u16)(p[0] << 8 | p[1]) : (u16)(p[1] << 8 | p[0]);
}

static u32 tiff_u32(const Tiff* t, size_t pos)
{
    const u8* p = t->file + pos;
    return t->big_endian ? ((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3])
                         : ((u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]);
}

static f64 tiff_f64(const Tiff* t, size_t pos)
{
    u64 lo = tiff_u32(t, pos + (t->big_endian ? 4 : 0)), hi = tiff_u32(t, pos + (t->big_endian ? 0 : 4));
    u64 bits = hi << 32 | lo;
    f64 v;
    memcpy(&v, &bits, 8);
    return v;
}

// Value i of a SHORT or LONG array starting at pos.
static u32 tiff_index(const Tiff* t, u32 type, size_t pos, u32 i)
{
    return type == 3 ? tiff_u16(t, pos + 2 * i) : tiff_u32(t, pos + 4 * i);
}

// Returns an error string or NULL.
static const char* tiff_open(Tiff* t, const u8* file, size_t size)
{
    memset(t, 0, sizeof(*t));
    t->file = file; t->file_size = size;
    t->compression = 1; t->predictor = 1; t->sample_format = 1; t->bits = 8;
    if (size < 8) return "too small";
    if (file[0] == 'I' && file[1] == 'I') t->big_endian = false;
    else if (file[0] == 'M' && file[1] == 'M') t->big_endian = true;
    else return "not a TIFF";
    u16 magic = tiff_u16(t, 2);
    if (magic == 43) return "BigTIFF not supported (convert with gdal_translate)";
    if (magic != 42) return "not a TIFF";

    size_t ifd = tiff_u32(t, 4);
    if (ifd + 2 > size) return "bad IFD offset";
    u32 n = tiff_u16(t, ifd);
    if (ifd + 2 + 12 * (size_t)n > size) return "bad IFD";
    u32 rows_per_strip = 0xffffffffu;
    t->samples = 1; t->planar = 1; t->photometric = 1;
    f64 tie[6] = {}, scale[3] = {};
    bool has_tie = false, has_scale = false, pixel_is_point = false;
    for (u32 e = 0; e < n; e++) {
        size_t entry = ifd + 2 + 12 * (size_t)e;
        u32 tag = tiff_u16(t, entry), type = tiff_u16(t, entry + 2), count = tiff_u32(t, entry + 4);
        static const u32 type_size[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        size_t bytes = (size_t)count * (type < 13 ? type_size[type] : 1);
        size_t vpos = bytes <= 4 ? entry + 8 : tiff_u32(t, entry + 8);
        if (vpos + bytes > size) return "bad tag data";
        u32 v = type == 3 ? tiff_u16(t, vpos) : type == 4 ? tiff_u32(t, vpos) : 0;
        switch (tag) {
        case 256: t->width = v; break;
        case 257: t->height = v; break;
        case 258: t->bits = v; break;
        case 259: t->compression = v; break;
        case 273: case 324: t->chunk_offsets_pos = (u32)vpos; t->chunk_offsets_type = type; t->chunk_count = count; break;
        case 279: case 325: t->chunk_counts_pos = (u32)vpos; t->chunk_counts_type = type; break;
        case 262: t->photometric = v; break;
        case 277: t->samples = v; break;
        case 278: rows_per_strip = v; break;
        case 284: t->planar = v; break;
        case 317: t->predictor = v; break;
        case 322: t->tile_w = v; break;
        case 323: t->tile_h = v; break;
        case 338: t->extra_samples = count; break;
        case 339: t->sample_format = v; break;
        case 33550: if (count >= 3) { for (int i = 0; i < 3; i++) scale[i] = tiff_f64(t, vpos + 8 * i); has_scale = true; } break;
        case 33922: if (count >= 6) { for (int i = 0; i < 6; i++) tie[i] = tiff_f64(t, vpos + 8 * i); has_tie = true; } break;
        case 34264: return "ModelTransformation not supported";
        case 34735:   // GeoKeyDirectory: header (4 shorts) + count * (key, location, count, value)
            for (u32 k = 1; k <= tiff_u16(t, vpos + 6) && 4 * (k + 1) <= count; k++) {
                u32 key = tiff_u16(t, vpos + 8 * k), loc = tiff_u16(t, vpos + 8 * k + 2), val = tiff_u16(t, vpos + 8 * k + 6);
                if (loc != 0) continue;
                if (key == 1025) pixel_is_point = val == 2;
                if (key == 3072) t->epsg = val;
            }
            break;
        case 42113: {   // GDAL_NODATA, ASCII
            char buf[64] = {};
            memcpy(buf, file + vpos, MIN(bytes, sizeof(buf) - 1));
            char* end;
            t->nodata = strtod(buf, &end);
            t->has_nodata = end != buf;
        } break;
        }
    }
    if (!t->width || !t->height || !t->chunk_count) return "missing image tags";
    if (!t->samples || (t->planar != 1 && t->planar != 2)) return "bad SamplesPerPixel / PlanarConfiguration";
    if (t->photometric == 2 || t->photometric == 3 || t->photometric == 6 || (t->samples >= 3 && t->bits == 8))
        return "colour image (RGB/palette/YCbCr), not an elevation raster: wrong product or URL?";
    if (t->compression != 1 && t->compression != 5 && t->compression != 8 && t->compression != 32946)
        return "unsupported compression (supported: none, LZW, Deflate)";
    if (t->predictor < 1 || t->predictor > 3) return "unsupported predictor";
    bool int_ok = t->sample_format <= 2 && (t->bits == 8 || t->bits == 16 || t->bits == 32);
    bool flt_ok = t->sample_format == 3 && (t->bits == 32 || t->bits == 64);
    if (!int_ok && !flt_ok) return "unsupported sample type";
    if (t->predictor == 3 && t->sample_format != 3) return "predictor 3 on integer data";
    if (!t->tile_w) { t->tile_w = t->width; t->tile_h = MIN(rows_per_strip, t->height); }
    if (!t->tile_h) return "bad tile size";
    t->chunks_per_band = ((t->width + t->tile_w - 1) / t->tile_w) * ((t->height + t->tile_h - 1) / t->tile_h);
    if (t->chunk_count < t->chunks_per_band * (t->planar == 2 ? t->samples : 1)) return "fewer strips/tiles than the image needs";
    if (has_tie && has_scale) {
        t->sx = scale[0]; t->sy = scale[1];
        f64 c = pixel_is_point ? 0.0 : 0.5;   // tiepoint refers to the pixel corner (area) or centre (point)
        t->x0 = tie[3] + (c - tie[0]) * t->sx;
        t->y0 = tie[4] - (c - tie[1]) * t->sy;
        t->has_geo = true;
    }
    return NULL;
}

// TIFF LZW (MSB-first codes, 9..12 bits, early change). Returns bytes written or -1.
static i64 tiff_lzw(const u8* in, size_t in_size, u8* out, size_t out_size)
{
    static thread_local u16 prefix[4096], length[4096];
    static thread_local u8 suffix[4096], first[4096];
    for (int i = 0; i < 256; i++) { prefix[i] = 0xffff; suffix[i] = first[i] = (u8)i; length[i] = 1; }
    size_t out_pos = 0, bitpos = 0, in_bits = in_size * 8;
    u32 width = 9, next = 258;
    int old = -1;
    for (;;) {
        if (bitpos + width > in_bits) break;   // truncated stream without EOI: accept what we have
        u32 code = 0;
        for (u32 b = 0; b < width; b++, bitpos++) code = code << 1 | ((in[bitpos >> 3] >> (7 - (bitpos & 7))) & 1);
        if (code == 257) break;
        if (code == 256) { width = 9; next = 258; old = -1; continue; }
        if (old < 0) {
            if (code > 255 || out_pos >= out_size) return -1;
            out[out_pos++] = (u8)code;
            old = (int)code;
            continue;
        }
        u32 str = code;
        u8 head;
        if (code < next) head = first[code];
        else if (code == next) { head = first[old]; str = (u32)old; }
        else return -1;
        // Emit string `str` (plus `head` again if code == next).
        u32 len = length[str], total = len + (code == next);
        if (out_pos + total > out_size) return -1;
        u32 c = str;
        for (u32 i = len; i-- > 0; c = prefix[c]) out[out_pos + i] = suffix[c];
        if (code == next) out[out_pos + len] = head;
        out_pos += total;
        if (next < 4096) {
            prefix[next] = (u16)old; suffix[next] = head; first[next] = first[old]; length[next] = (u16)(length[old] + 1);
            next++;
        }
        if (next + 1 >= (1u << width) && width < 12) width++;
        old = (int)code;
    }
    return (i64)out_pos;
}

// Decodes the whole raster into f32, row-major, row 0 = north (file order). NaN for nodata.
static const char* tiff_read_f32(const Tiff* t, f32* dst)
{
    u32 bps = t->bits / 8;
    u32 spp = t->planar == 1 ? t->samples : 1;     // samples per pixel inside one chunk
    u32 tiles_x = (t->width + t->tile_w - 1) / t->tile_w;
    size_t chunk_bytes = (size_t)t->tile_w * t->tile_h * spp * bps;
    u8* raw = (u8*)malloc(chunk_bytes);
    u8* tmp = (u8*)malloc(chunk_bytes);
    const char* err = NULL;
    for (u32 c = 0; c < t->chunks_per_band && !err; c++) {
        size_t off = tiff_index(t, t->chunk_offsets_type, t->chunk_offsets_pos, c);
        size_t len = tiff_index(t, t->chunk_counts_type, t->chunk_counts_pos, c);
        if (off + len > t->file_size) { err = "chunk out of file"; break; }
        u32 cx = (c % tiles_x) * t->tile_w, cy = (c / tiles_x) * t->tile_h;
        if (cy >= t->height) break;
        // Strips: the last one may be short. Tiles: always full size, cropped on copy.
        u32 rows = t->tile_w == t->width ? MIN(t->tile_h, t->height - cy) : t->tile_h;
        size_t want = (size_t)t->tile_w * rows * spp * bps;
        i64 got;
        if (t->compression == 1) { got = (i64)MIN(len, want); memcpy(raw, t->file + off, (size_t)got); }
        else if (t->compression == 5) got = tiff_lzw(t->file + off, len, raw, want);
        else got = inflate_zlib(t->file + off, len, raw, want);
        if (got != (i64)want) { err = "chunk decode failed"; break; }

        u32 row_samples = t->tile_w * spp;   // values per row; predictors difference with stride spp
        if (t->predictor == 3) {
            // Floating-point predictor: bytes differenced along the row, then stored as byte planes (MSB first).
            for (u32 r = 0; r < rows; r++) {
                u8* row = raw + (size_t)r * row_samples * bps;
                for (u32 i = spp; i < row_samples * bps; i++) row[i] = (u8)(row[i] + row[i - spp]);
                u8* o = tmp + (size_t)r * row_samples * bps;
                for (u32 i = 0; i < row_samples; i++)
                    for (u32 b = 0; b < bps; b++) o[bps * i + b] = row[(bps - 1 - b) * row_samples + i];   // to little endian
            }
            memcpy(raw, tmp, want);
        } else if (t->big_endian && bps > 1) {
            for (size_t i = 0; i < want; i += bps)
                for (u32 b = 0; b < bps / 2; b++) { u8 x = raw[i + b]; raw[i + b] = raw[i + bps - 1 - b]; raw[i + bps - 1 - b] = x; }
        }
        if (t->predictor == 2) {
            for (u32 r = 0; r < rows; r++) {
                u8* row = raw + (size_t)r * row_samples * bps;
                for (u32 i = spp; i < row_samples; i++) {
                    u32 j = i - spp;
                    if (bps == 1) row[i] = (u8)(row[i] + row[j]);
                    else if (bps == 2) { u16 a, b; memcpy(&a, row + 2 * i, 2); memcpy(&b, row + 2 * j, 2); a = (u16)(a + b); memcpy(row + 2 * i, &a, 2); }
                    else { u32 a, b; memcpy(&a, row + 4 * i, 4); memcpy(&b, row + 4 * j, 4); a += b; memcpy(row + 4 * i, &a, 4); }
                }
            }
        }
        for (u32 r = 0; r < rows && cy + r < t->height; r++) {
            for (u32 i = 0; i < t->tile_w && cx + i < t->width; i++) {
                const u8* p = raw + ((size_t)r * row_samples + (size_t)i * spp) * bps;   // band 0
                f64 v;
                if (t->sample_format == 3) {
                    if (bps == 4) { f32 f; memcpy(&f, p, 4); v = f; } else memcpy(&v, p, 8);
                } else if (t->sample_format == 2) {
                    v = bps == 1 ? (f64)(i8)p[0] : bps == 2 ? (f64)(i16)(p[0] | p[1] << 8) : (f64)(i32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24);
                } else {
                    v = bps == 1 ? (f64)p[0] : bps == 2 ? (f64)(u16)(p[0] | p[1] << 8) : (f64)(u32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24);
                }
                if (t->has_nodata && v == t->nodata) v = NAN;
                dst[(size_t)(cy + r) * t->width + cx + i] = (f32)v;
            }
        }
    }
    free(raw);
    free(tmp);
    return err;
}

// One-line layout summary for error messages.
static void tiff_describe(const Tiff* t, char* buf, size_t size)
{
    snprintf(buf, size, "%ux%u, %u sample(s)/pixel (%u extra), %u-bit %s, photometric %u, planar %u, compression %u, predictor %u, %s %ux%u",
             t->width, t->height, t->samples, t->extra_samples, t->bits,
             t->sample_format == 3 ? "float" : t->sample_format == 2 ? "int" : "uint", t->photometric, t->planar,
             t->compression, t->predictor, t->tile_w == t->width ? "strips" : "tiles", t->tile_w, t->tile_h);
}

// Prints every IFD entry of every IFD (for diagnosing files the reader rejects).
static void tiff_dump(const u8* file, size_t size)
{
    Tiff t = {};
    t.file = file; t.file_size = size;
    if (size < 8 || !((file[0] == 'I' && file[1] == 'I') || (file[0] == 'M' && file[1] == 'M'))) { printf("not a TIFF\n"); return; }
    t.big_endian = file[0] == 'M';
    printf("byte order %s, magic %u\n", t.big_endian ? "MM" : "II", tiff_u16(&t, 2));
    if (tiff_u16(&t, 2) != 42) return;
    static const u32 type_size[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    size_t ifd = tiff_u32(&t, 4);
    for (u32 index = 0; ifd && ifd + 2 <= size && index < 16; index++) {
        u32 n = tiff_u16(&t, ifd);
        printf("IFD %u: %u entries\n", index, n);
        for (u32 e = 0; e < n && ifd + 2 + 12 * (size_t)(e + 1) <= size; e++) {
            size_t entry = ifd + 2 + 12 * (size_t)e;
            u32 tag = tiff_u16(&t, entry), type = tiff_u16(&t, entry + 2), count = tiff_u32(&t, entry + 4);
            size_t bytes = (size_t)count * (type < 13 ? type_size[type] : 1);
            size_t vpos = bytes <= 4 ? entry + 8 : tiff_u32(&t, entry + 8);
            printf("  tag %5u type %2u count %7u:", tag, type, count);
            if (vpos + bytes > size) { printf(" <out of file>\n"); continue; }
            if (type == 2) { printf(" \"%.*s\"\n", (int)MIN(bytes, (size_t)200), (const char*)file + vpos); continue; }
            for (u32 i = 0; i < MIN(count, 12u); i++) {
                if (type == 3) printf(" %u", tiff_u16(&t, vpos + 2 * i));
                else if (type == 4) printf(" %u", tiff_u32(&t, vpos + 4 * i));
                else if (type == 12) printf(" %.6f", tiff_f64(&t, vpos + 8 * i));
                else if (type == 1 || type == 7) printf(" %u", file[vpos + i]);
                else { printf(" ..."); break; }
            }
            printf(count > 12 ? " ...\n" : "\n");
        }
        ifd = ifd + 2 + 12 * (size_t)n + 4 <= size ? tiff_u32(&t, ifd + 2 + 12 * (size_t)n) : 0;
    }
}
