// Reference terrain: cooked heightfield -> GPU texture + CDLOD quadtree (Strugar 2010), selected on the CPU each
// frame. Interim for M1: the production path selects patches on the GPU and rasterizes them as procedural clusters
// into the visibility buffer (docs/12, docs/05); patch size, morphing and the height function stay the same.

#define TERRAIN_MAX_NODES 65536

typedef struct {
    TerrainFileHeader hdr;
    u16* heights;                       // CPU copy: the shared height function for camera/gameplay (docs/12)
    u32 levels;                         // quadtree levels; root = levels - 1
    u32 nodes_x[TERRAIN_MAX_LEVELS], nodes_z[TERRAIN_MAX_LEVELS];
    u16* minmax[TERRAIN_MAX_LEVELS];    // per node: min, max (u16 height units)
    f32 range[TERRAIN_MAX_LEVELS];      // LOD distance per level (m)

    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    VkBuf indices;
    u32 index_count;
    VkPipeline pipeline, pipeline_wire, pipeline_sky;
} Terrain;

typedef struct {
    v3 cam;                             // relative to render origin
    v3 planes_n[5];                     // inward normals: near, left, right, bottom, top (through cam)
    f32 near_d;
    v3 origin_terrain;                  // terrain sample (0,0) relative to render origin, y = height_min - origin.y
    TerrainNode* out;
    u32 count;
} TerrainSelect;

static void terrain_load(Terrain* t, const char* path)
{
    size_t size;
    u8* file = (u8*)read_file(path, &size);
    if (!file) FATAL("cannot read %s (cook it first: see README, 'Running the viewer')", path);
    if (size < sizeof(TerrainFileHeader)) FATAL("%s: too small", path);
    memcpy(&t->hdr, file, sizeof(t->hdr));
    const TerrainFileHeader* h = &t->hdr;
    if (h->magic != TERRAIN_FILE_MAGIC || h->version != TERRAIN_FILE_VERSION) FATAL("%s: not a VRH%u file", path, TERRAIN_FILE_VERSION);
    if (size != sizeof(TerrainFileHeader) + (size_t)h->width * h->height * 2) FATAL("%s: size mismatch", path);
    t->heights = (u16*)(file + sizeof(TerrainFileHeader));   // file buffer stays alive
    printf("terrain: %u x %u samples, %.2f m, %.1f..%.1f m, origin E %.1f N %.1f (EPSG:%u), %u samples hole-filled\n",
           h->width, h->height, h->spacing, h->height_min, h->height_min + 65535.0f * h->height_scale,
           h->origin_e, h->origin_n, h->epsg, h->holes_filled);

    // Min/max per node. Level 0 node (i,j) covers samples [8i, 8i+8] in both axes.
    u32 span = MAX(h->width, h->height) - 1;
    t->levels = 1;
    while (((u32)TERRAIN_PATCH_QUADS << (t->levels - 1)) < span) t->levels++;
    if (t->levels > TERRAIN_MAX_LEVELS) FATAL("terrain too large for %u levels", TERRAIN_MAX_LEVELS);
    for (u32 l = 0; l < t->levels; l++) {
        u32 ns = TERRAIN_PATCH_QUADS << l;
        t->nodes_x[l] = (h->width - 1 + ns - 1) / ns;
        t->nodes_z[l] = (h->height - 1 + ns - 1) / ns;
        t->minmax[l] = (u16*)malloc((size_t)t->nodes_x[l] * t->nodes_z[l] * 2 * sizeof(u16));
    }
    for (u32 j = 0; j < t->nodes_z[0]; j++)
        for (u32 i = 0; i < t->nodes_x[0]; i++) {
            u16 lo = 0xffff, hi = 0;
            for (u32 z = j * TERRAIN_PATCH_QUADS; z <= MIN((j + 1) * TERRAIN_PATCH_QUADS, h->height - 1); z++)
                for (u32 x = i * TERRAIN_PATCH_QUADS; x <= MIN((i + 1) * TERRAIN_PATCH_QUADS, h->width - 1); x++) {
                    u16 v = t->heights[(size_t)z * h->width + x];
                    lo = MIN(lo, v); hi = MAX(hi, v);
                }
            u16* m = &t->minmax[0][2 * ((size_t)j * t->nodes_x[0] + i)];
            m[0] = lo; m[1] = hi;
        }
    for (u32 l = 1; l < t->levels; l++)
        for (u32 j = 0; j < t->nodes_z[l]; j++)
            for (u32 i = 0; i < t->nodes_x[l]; i++) {
                u16 lo = 0xffff, hi = 0;
                for (u32 c = 0; c < 4; c++) {
                    u32 ci = 2 * i + (c & 1), cj = 2 * j + (c >> 1);
                    if (ci >= t->nodes_x[l - 1] || cj >= t->nodes_z[l - 1]) continue;
                    const u16* m = &t->minmax[l - 1][2 * ((size_t)cj * t->nodes_x[l - 1] + ci)];
                    lo = MIN(lo, m[0]); hi = MAX(hi, m[1]);
                }
                u16* m = &t->minmax[l][2 * ((size_t)j * t->nodes_x[l] + i)];
                m[0] = lo; m[1] = hi;
            }
}

// Bilinear height in metres at local position (x east, z north, metres from sample (0,0)); clamps at the edges.
static f32 terrain_height(const Terrain* t, f64 x, f64 z)
{
    const TerrainFileHeader* h = &t->hdr;
    f64 gx = CLAMP(x / h->spacing, 0.0, (f64)(h->width - 1)), gz = CLAMP(z / h->spacing, 0.0, (f64)(h->height - 1));
    u32 x0 = MIN((u32)gx, h->width - 2), z0 = MIN((u32)gz, h->height - 2);
    f32 fx = (f32)(gx - x0), fz = (f32)(gz - z0);
    const u16* r0 = t->heights + (size_t)z0 * h->width + x0;
    const u16* r1 = r0 + h->width;
    f32 v = (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fz) + (r1[0] * (1 - fx) + r1[1] * fx) * fz;
    return h->height_min + v * h->height_scale;
}

static void terrain_upload(Terrain* t, Vk* vk)
{
    const TerrainFileHeader* h = &t->hdr;
    if (MAX(h->width, h->height) > vk->props.limits.maxImageDimension2D)
        FATAL("terrain %u x %u exceeds maxImageDimension2D %u; cook with --step 2", h->width, h->height, vk->props.limits.maxImageDimension2D);

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk->phys, VK_FORMAT_R16_UNORM, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    u32 mips = 1;
    if ((fp.optimalTilingFeatures & need) == need)
        while ((MAX(h->width, h->height) >> mips) > 0) mips++;

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16_UNORM;
    ci.extent = {h->width, h->height, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &t->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, t->image, &req);
    t->image_memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
    VK_CHECK(vkBindImageMemory(vk->device, t->image, t->image_memory, 0));

    // Upload level 0 in row bands through a staging buffer.
    VkDeviceSize staging_size = 64u << 20;
    VkBuf staging = vk_buffer(vk, staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    u32 rows_per_band = (u32)(staging_size / ((VkDeviceSize)h->width * 2));
    for (u32 row = 0; row < h->height; row += rows_per_band) {
        u32 rows = MIN(rows_per_band, h->height - row);
        memcpy(staging.mapped, t->heights + (size_t)row * h->width, (size_t)rows * h->width * 2);
        VkCommandBuffer cmd = vk_begin_once(vk);
        if (row == 0)
            vk_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, (i32)row, 0};
        region.imageExtent = {h->width, rows, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vk_end_once(vk);
    }
    vk_buffer_destroy(vk, &staging);

    // Mips by successive linear blits (used for footprint-filtered normals in the fragment shader).
    VkCommandBuffer cmd = vk_begin_once(vk);
    for (u32 m = 1; m < mips; m++) {
        vk_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit b = {};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        b.srcOffsets[1] = {(i32)MAX(h->width >> (m - 1), 1u), (i32)MAX(h->height >> (m - 1), 1u), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        b.dstOffsets[1] = {(i32)MAX(h->width >> m, 1u), (i32)MAX(h->height >> m, 1u), 1};
        vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (mips > 1)
        vk_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips - 1, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_end_once(vk);

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &t->view));
    vk_bind_texture(vk, 0, t->view);

    // One shared patch index buffer; vertices are generated from gl_VertexIndex.
    const u32 q = TERRAIN_PATCH_QUADS, row = q + 1;
    t->index_count = q * q * 6;
    t->indices = vk_buffer(vk, t->index_count * sizeof(u16), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    u16* idx = (u16*)t->indices.mapped;
    for (u32 z = 0; z < q; z++)
        for (u32 x = 0; x < q; x++) {
            u16 a = (u16)(z * row + x), b = (u16)(a + 1), c = (u16)(a + row), d = (u16)(c + 1);
            // Alternate the diagonal so slopes do not show a uniform grain.
            if ((x ^ z) & 1) { *idx++ = a; *idx++ = b; *idx++ = d; *idx++ = a; *idx++ = d; *idx++ = c; }
            else             { *idx++ = a; *idx++ = b; *idx++ = c; *idx++ = b; *idx++ = d; *idx++ = c; }
        }
}

static void terrain_create_pipelines(Terrain* t, Vk* vk, const char* shader_path)
{
    VkShaderModule m = vk_load_shader(vk, shader_path);
    PipelineDesc d = {m, "vs_terrain", "fs_terrain", VK_COMPARE_OP_GREATER, true, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL};
    t->pipeline = vk_create_pipeline(vk, &d);
    if (vk->wireframe_supported) {
        d.polygon = VK_POLYGON_MODE_LINE;
        t->pipeline_wire = vk_create_pipeline(vk, &d);
    }
    PipelineDesc s = {m, "vs_sky", "fs_sky", VK_COMPARE_OP_GREATER_OR_EQUAL, false, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL};
    t->pipeline_sky = vk_create_pipeline(vk, &s);
    vkDestroyShaderModule(vk->device, m, NULL);
}

// LOD ranges from a target triangle edge in pixels; each level doubles. Clamped so that a node selected at level L
// (which may extend up to its parent's diagonal beyond range[L]) never reaches the morph zone of level L + 1.
static void terrain_set_ranges(Terrain* t, f32 proj_y_pixels, f32 target_px, FrameConstants* fc)
{
    for (u32 l = 0; l < TERRAIN_MAX_LEVELS; l++) {
        f32 edge = t->hdr.spacing * (f32)(1u << MIN(l, 30u));
        f32 node = edge * TERRAIN_PATCH_QUADS;
        t->range[l] = MAX(edge * proj_y_pixels / target_px, 8.0f * node);
    }
    for (u32 l = 0; l < TERRAIN_MAX_LEVELS; l++) {
        f32 prev = l ? t->range[l - 1] : 0.0f;
        f32 start = prev + (t->range[l] - prev) * 0.66f;
        if (l + 1 >= t->levels) start = 1e30f;   // root level never morphs
        fc->morph[l] = {start, 1.0f / MAX(t->range[l] - start, 1e-3f), 0, 0};
    }
}

static bool aabb_in_frustum(const TerrainSelect* s, v3 lo, v3 hi)
{
    for (u32 i = 0; i < 5; i++) {
        v3 n = s->planes_n[i];
        v3 p = v3_make(n.x >= 0 ? hi.x : lo.x, n.y >= 0 ? hi.y : lo.y, n.z >= 0 ? hi.z : lo.z);
        f32 d = v3_dot(n, v3_sub(p, s->cam)) - (i == 0 ? s->near_d : 0.0f);
        if (d < 0) return false;
    }
    return true;
}

static bool aabb_in_sphere(v3 c, f32 r, v3 lo, v3 hi)
{
    f32 dx = MAX(MAX(lo.x - c.x, 0.0f), c.x - hi.x);
    f32 dy = MAX(MAX(lo.y - c.y, 0.0f), c.y - hi.y);
    f32 dz = MAX(MAX(lo.z - c.z, 0.0f), c.z - hi.z);
    return dx * dx + dy * dy + dz * dz <= r * r;
}

static void terrain_select_node(const Terrain* t, TerrainSelect* s, u32 level, u32 i, u32 j)
{
    const TerrainFileHeader* h = &t->hdr;
    if (i >= t->nodes_x[level] || j >= t->nodes_z[level]) return;
    u32 ns = TERRAIN_PATCH_QUADS << level;
    u32 gx = i * ns, gz = j * ns;
    const u16* m = &t->minmax[level][2 * ((size_t)j * t->nodes_x[level] + i)];
    v3 lo = v3_make(s->origin_terrain.x + gx * h->spacing, s->origin_terrain.y + m[0] * h->height_scale, s->origin_terrain.z + gz * h->spacing);
    v3 hi = v3_make(s->origin_terrain.x + MIN(gx + ns, h->width - 1) * h->spacing, s->origin_terrain.y + m[1] * h->height_scale,
                    s->origin_terrain.z + MIN(gz + ns, h->height - 1) * h->spacing);
    if (!aabb_in_frustum(s, lo, hi)) return;
    if (level == 0 || !aabb_in_sphere(s->cam, t->range[level - 1], lo, hi)) {
        if (s->count < TERRAIN_MAX_NODES) s->out[s->count++] = {gx, gz, level, 0};
        return;
    }
    for (u32 c = 0; c < 4; c++) terrain_select_node(t, s, level - 1, 2 * i + (c & 1), 2 * j + (c >> 1));
}
