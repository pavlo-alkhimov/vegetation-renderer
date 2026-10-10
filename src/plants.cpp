// Near-field ground cover: cooked Poly Haven plants (cook_plants -> plants.vrp) as meshlets. Placement happens on
// the GPU every frame (shaders/plants.slang); the CPU only loads the assets and sets up the dispatch grid.

static_assert(sizeof(PlantVariant) == 48 && sizeof(PlantSpecies) == 48, "GPU struct size");

// Placement and look per Poly Haven asset. Density in plants per m² for meadow / forest edge / forest floor (the
// habitat weights come from the vegetation mask, shaders/plants.slang). The assets are small (grass tufts
// 0.04-0.4 m, young nettles 0.2 m), so the scale ranges bring them to meadow size. Species with a draw distance up to
// PLANT_NEAR_DISTANCE are placed in the dense near layer.
static const struct {
    const char* name;
    f32 meadow, edge, floor;
    f32 scale_min, scale_max;
    f32 distance;           // m
    f32 wind;
} PLANT_RULES[] = {
    {"grass_medium_01",  30.0f, 16.0f, 3.0f, 1.2f, 2.2f, 15.0f, 0.9f},
    {"grass_medium_02",  14.0f,  8.0f, 1.5f, 1.0f, 1.8f, 15.0f, 0.9f},
    {"weed_plant_02",     2.0f,  2.0f, 1.0f, 1.0f, 1.5f, 15.0f, 0.4f},
    {"celandine_01",      0.0f,  3.0f, 8.0f, 0.8f, 1.2f, 15.0f, 0.3f},
    {"periwinkle_plant",  0.0f,  1.0f, 6.0f, 0.5f, 0.8f, 15.0f, 0.3f},
    {"dandelion_01",      1.5f,  0.5f, 0.0f, 1.0f, 1.6f, 30.0f, 0.4f},
    {"nettle_plant",      0.0f,  3.0f, 0.4f, 3.0f, 5.0f, 40.0f, 0.5f},
    {"fern_02",           0.0f,  1.0f, 1.5f, 1.8f, 2.6f, 40.0f, 0.5f},
};
typedef struct {
    bool enabled;
    u32 species_count, variant_count;
    f32 max_distance;               // m, largest species draw distance
    VkTex tex[PLANT_MAX_SPECIES][2];
    VkBuf scene, variant_buf, species_buf, meshlet_buf, vertex_buf, triangle_buf;
    VkPipeline pipeline;
    // Per frame
    u32 cells;                      // dispatch grid side
} Plants;

// path: cooked file; shader_dir with trailing separator. Leaves p->enabled false if the file or mesh shaders are
// missing.
static void plants_init(Plants* p, Vk* vk, const char* path, const char* shader_dir)
{
    if (!vk->mesh_shaders) return;
    u64 t0 = SDL_GetPerformanceCounter();
    size_t size = 0;
    u8* file = (u8*)read_file(path, &size);
    if (!file) { printf("plants: %s not found (tools/fetch-polyhaven.sh + cook_plants): near-field plants off\n", path); return; }
    const PlantsFileHeader* h = (const PlantsFileHeader*)file;
    if (size < sizeof(*h) || h->magic != PLANTS_FILE_MAGIC || h->version != PLANTS_FILE_VERSION) FATAL("%s: not a plants file (or old version)", path);
    if (h->species_count > PLANT_MAX_SPECIES) FATAL("%s: too many species", path);
    const PlantsFileSpecies* fsp = (const PlantsFileSpecies*)(h + 1);
    const PlantsFileVariant* fvar = (const PlantsFileVariant*)(fsp + h->species_count);
    const TreeVertex* fverts = (const TreeVertex*)(fvar + h->variant_count);
    const u32* findex = (const u32*)(fverts + h->vertex_count);
    const u8* ftex = (const u8*)(findex + h->index_count);
    size_t tex_bytes = 0;
    for (u32 m = 0; m < h->tex_mips; m++) tex_bytes += (size_t)(h->tex_size >> m) * (h->tex_size >> m) * 4;
    if ((size_t)(ftex - file) + tex_bytes * 2 * h->species_count != size) FATAL("%s: size mismatch", path);

    // Variants x LODs -> meshlets (vertices compacted per mesh, see cook_plants: every LOD has its own copy).
    PlantVariant* variants = (PlantVariant*)calloc(h->variant_count, sizeof(PlantVariant));
    MeshBuild mb = {};
    MeshletArrays ma = {};
    i32* remap = (i32*)malloc(MAX(h->vertex_count, 1u) * sizeof(i32));
    for (u32 i = 0; i < h->vertex_count; i++) remap[i] = -1;
    u64 tris[PLANT_LODS] = {};
    for (u32 v = 0; v < h->variant_count; v++) {
        const PlantsFileVariant* fv = &fvar[v];
        PlantVariant* pv = &variants[v];
        pv->species = fv->species;
        pv->height = fv->height;
        pv->radius = fv->radius;
        for (u32 lod = 0; lod < PLANT_LODS; lod++) {
            mb.vn = mb.tn = 0;
            const u32* idx = findex + fv->first_index[lod];
            for (u32 i = 0; i < fv->index_count[lod]; i++) {
                u32 g = idx[i];
                if (g >= h->vertex_count) FATAL("%s: index out of range", path);
                if (remap[g] < 0) {
                    if (mb.vn == mb.vcap) { mb.vcap = mb.vcap ? mb.vcap * 2 : 4096; mb.v = (TreeVertex*)realloc(mb.v, mb.vcap * sizeof(TreeVertex)); }
                    remap[g] = (i32)mb.vn;
                    mb.v[mb.vn++] = fverts[g];
                }
                if (mb.tn == mb.tcap) { mb.tcap = mb.tcap ? mb.tcap * 2 : 12288; mb.t = (u32*)realloc(mb.t, mb.tcap * sizeof(u32)); }
                mb.t[mb.tn++] = (u32)remap[g];
            }
            for (u32 i = 0; i < fv->index_count[lod]; i++) remap[idx[i]] = -1;
            meshletize(&ma, &mb, &pv->meshlet_first[lod], &pv->meshlet_count[lod]);
            tris[lod] += mb.tn / 3;
        }
    }
    free(remap);
    free(mb.v);
    free(mb.t);

    PlantSpecies species[PLANT_MAX_SPECIES] = {};
    p->max_distance = 0;
    for (u32 s = 0; s < h->species_count; s++) {
        PlantSpecies* ps = &species[s];
        ps->first_variant = fsp[s].first_variant;
        ps->variant_count = fsp[s].variant_count;
        ps->albedo_tex = TEX_PLANTS + 2 * s;
        ps->surface_tex = TEX_PLANTS + 2 * s + 1;
        ps->habitat.w = fsp[s].has_alpha ? 1.0f : 0.0f;
        ps->shape = {1, 1, 0, 0};
        for (u32 r = 0; r < ARRAY_COUNT(PLANT_RULES); r++) {
            if (strcmp(PLANT_RULES[r].name, fsp[s].name)) continue;
            ps->habitat.x = PLANT_RULES[r].meadow;
            ps->habitat.y = PLANT_RULES[r].edge;
            ps->habitat.z = PLANT_RULES[r].floor;
            ps->shape = {PLANT_RULES[r].scale_min, PLANT_RULES[r].scale_max, PLANT_RULES[r].distance, PLANT_RULES[r].wind};
        }
        if (ps->shape.z == 0) printf("plants: no placement rule for %s, not placed\n", fsp[s].name);
        p->max_distance = MAX(p->max_distance, ps->shape.z);
        const u8* t = ftex + tex_bytes * 2 * s;
        p->tex[s][0] = vk_texture_2d_levels(vk, VK_FORMAT_R8G8B8A8_SRGB, 4, h->tex_size, h->tex_mips, t);
        p->tex[s][1] = vk_texture_2d_levels(vk, VK_FORMAT_R8G8B8A8_UNORM, 4, h->tex_size, h->tex_mips, t + tex_bytes);
        vk_bind_texture(vk, ps->albedo_tex, p->tex[s][0].view);
        vk_bind_texture(vk, ps->surface_tex, p->tex[s][1].view);
    }

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    p->variant_buf = vk_buffer_static(vk, (VkDeviceSize)h->variant_count * sizeof(PlantVariant), usage, variants);
    p->species_buf = vk_buffer_static(vk, sizeof(species), usage, species);
    p->meshlet_buf = vk_buffer_static(vk, (VkDeviceSize)ma.mn * sizeof(TreeMeshlet), usage, ma.m);
    p->vertex_buf = vk_buffer_static(vk, (VkDeviceSize)ma.vn * sizeof(TreeVertex), usage, ma.v);
    p->triangle_buf = vk_buffer_static(vk, (VkDeviceSize)ma.tn * sizeof(u32), usage, ma.t);
    PlantScene scene = {p->variant_buf.address, p->species_buf.address, p->meshlet_buf.address, p->vertex_buf.address,
                        p->triangle_buf.address, h->species_count, 0};
    p->scene = vk_buffer_static(vk, sizeof(scene), usage, &scene);
    p->species_count = h->species_count;
    p->variant_count = h->variant_count;
    printf("plants: %u species, %u variants, triangles per LOD %llu / %llu / %llu, %u meshlets (%.1f MB), textures %u^2 (%.0f MB), %.0f ms\n",
           h->species_count, h->variant_count, (unsigned long long)tris[0], (unsigned long long)tris[1], (unsigned long long)tris[2], ma.mn,
           (ma.vn * sizeof(TreeVertex) + ma.tn * 4 + ma.mn * sizeof(TreeMeshlet)) / 1048576.0, h->tex_size,
           tex_bytes * 2 * h->species_count / 1048576.0, (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency());
    free(ma.v);
    free(ma.t);
    free(ma.m);
    free(variants);
    free(file);

    p->pipeline = veg_pipeline(vk, shader_dir, "plants");
    p->enabled = true;
}

// Dispatch grids around the camera (terrain-local metres), one per layer; p->cells = dispatch side.
static void plants_frame(Plants* p, const f64 cam_local[3], FrameConstants* fc)
{
    f32 radius[PLANT_LAYERS] = {(f32)PLANT_NEAR_DISTANCE, p->max_distance};
    p->cells = 0;
    for (u32 l = 0; l < PLANT_LAYERS; l++) {
        u32 n = 2 * (u32)ceilf(radius[l] / PLANT_LAYER_CELL[l]) + 2;
        fc->plant_cells[l] = n;
        fc->plant_cell_x[l] = (i32)floor(cam_local[0] / PLANT_LAYER_CELL[l]) - (i32)(n / 2);
        fc->plant_cell_z[l] = (i32)floor(cam_local[2] / PLANT_LAYER_CELL[l]) - (i32)(n / 2);
        p->cells = MAX(p->cells, n);
    }
}