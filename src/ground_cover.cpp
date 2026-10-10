// Near-field ground cover: cooked Poly Haven cover (cook_ground_cover -> ground_cover.vgc) as meshlets. Placement happens on
// the GPU every frame (shaders/ground_cover.slang); the CPU only loads the assets and sets up the dispatch grid.

#define GC_GREENNESS 0.6f          // FrameConstants.cover.w (summer; seasons will drive it)

static_assert(sizeof(GcVariant) == 48 && sizeof(GcSpecies) == 48, "GPU struct size");

// Placement and look per Poly Haven asset. Density in cover per m² for meadow / forest edge / forest floor (the
// habitat weights come from the vegetation mask, shaders/ground_cover.slang). The assets are small (grass tufts
// 0.04-0.4 m, young nettles 0.2 m), so the scale ranges bring them to meadow size. Species with a draw distance up to
// GC_NEAR_DISTANCE are placed in the dense near layer.
static const struct {
    const char* name;
    f32 meadow, edge, floor;
    f32 scale_min, scale_max;
    f32 distance;           // m
    f32 wind;
} GC_RULES[] = {
    {"grass_medium_01",  60.0f, 30.0f, 4.0f, 1.2f, 2.2f, 15.0f, 0.9f},
    {"grass_medium_02",  28.0f, 14.0f, 2.0f, 1.0f, 1.8f, 15.0f, 0.9f},
    {"weed_plant_02",     2.0f,  2.0f, 1.0f, 1.0f, 1.5f, 15.0f, 0.4f},
    {"celandine_01",      0.0f,  3.0f, 8.0f, 0.8f, 1.2f, 15.0f, 0.3f},
    {"periwinkle_plant",  0.0f,  1.0f, 6.0f, 0.5f, 0.8f, 15.0f, 0.3f},
    {"dandelion_01",      1.5f,  0.5f, 0.0f, 1.0f, 1.6f, 30.0f, 0.4f},
    {"nettle_plant",      0.0f,  1.5f, 0.3f, 3.0f, 5.0f, 40.0f, 0.5f},
    {"fern_02",           0.0f,  1.0f, 1.5f, 1.8f, 2.6f, 40.0f, 0.5f},
};
typedef struct {
    bool enabled;
    u32 species_count, variant_count;
    f32 max_distance;               // m, largest species draw distance
    v3 grass_color;                 // mean linear albedo of the meadow grasses' living texels (the terrain matches it)
    VkTex tex[GC_MAX_SPECIES][2];
    VkBuf scene, variant_buf, species_buf, meshlet_buf, vertex_buf, triangle_buf;
    VkPipeline pipeline;
    VkTex bake[GC_HABITATS][2];     // baked top view per habitat: colour (premultiplied, a = coverage), surface
    // Per frame
    u32 cells;                      // dispatch grid side
} GroundCover;

// Top view of each habitat's ground cover, rendered once at load with the near-field shaders: a GC_BAKE_TILE tile on
// flat ground (instances repeat with the tile period, so it tiles seamlessly), every instance at LOD 0, no wind.
// Colour is premultiplied by coverage (cleared to 0 = bare ground), so the mips average correctly; the terrain
// shows it under and beyond the meshes.
#define GC_BAKE_PASSES 3

static void gc_bake(GroundCover* p, Vk* vk, const char* shader_dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%sveg_gc_task.spv", shader_dir);
    VkShaderModule task = vk_load_shader(vk, path);
    snprintf(path, sizeof(path), "%sveg_gc.spv", shader_dir);
    VkShaderModule mesh = vk_load_shader(vk, path);
    PipelineDesc d = {mesh, NULL, "fs_gc_bake", VK_COMPARE_OP_GREATER, true, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, "as_gc", "ms_gc", task};
    const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    d.color_count = 2;
    d.color_formats[0] = formats[0];
    d.color_formats[1] = formats[1];
    d.with_depth = true;
    VkPipeline pipe = vk_create_pipeline(vk, &d);
    vkDestroyShaderModule(vk->device, task, NULL);
    vkDestroyShaderModule(vk->device, mesh, NULL);

    const u32 size = GC_BAKE_SIZE;
    u32 mips = 0;
    while ((size >> mips) > 0) mips++;
    VkTarget depth = vk_target(vk, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, {size, size});
    const f32 tile = (f32)GC_BAKE_TILE, top = 3.0f;      // m: tile side, height range of the depth
    FrameConstants* fc = (FrameConstants*)vk->frames[0].upload.mapped;
    ViewConstants* view = (ViewConstants*)((u8*)vk->frames[0].upload.mapped + UPLOAD_VIEWS_OFFSET);
    memset(view, 0, sizeof(*view));
    view->view_proj[0] = {2.0f / tile, 0, 0, -1};        // x -> u, z -> v, higher -> larger depth (reversed Z)
    view->view_proj[1] = {0, 0, 2.0f / tile, -1};
    view->view_proj[2] = {0, 1.0f / top, 0, 0.05f / top};
    view->view_proj[3] = {0, 0, 0, 1};
    memcpy(view->prev_view_proj, view->view_proj, sizeof(view->view_proj));
    view->planes[0] = {1, 0, 0, 2};                      // instances up to 2 m beyond the tile edges overlap it
    view->planes[1] = {-1, 0, 0, tile + 2};
    view->planes[2] = {0, 0, 1, 2};
    view->planes[3] = {0, 0, -1, tile + 2};
    view->planes[4] = {0, 1, 0, 1};
    view->planes[5] = {0, 0, 0, 1};
    PushConstants pcs = {};
    pcs.frame = vk->frames[0].upload.address;
    pcs.view = vk->frames[0].upload.address + UPLOAD_VIEWS_OFFSET;
    pcs.gc = p->scene.address;

    for (u32 hab = 0; hab < GC_HABITATS; hab++) {
        memset(fc, 0, sizeof(*fc));
        fc->cam_pos = {tile * 0.5f, 50, tile * 0.5f, 0.05f};
        fc->terrain = {0, 0, 1, 0};
        fc->terrain_size = {1e6f, 1e6f, 1, 0};
        fc->veg = {0, 0, 1000, 0};
        fc->wind = {1, 0, 0, 0};                         // strength 0: no bend, no flutter
        fc->cover = {p->grass_color.x, p->grass_color.y, p->grass_color.z, GC_GREENNESS};
        fc->gc_bake = {hab == 0 ? 1.0f : 0.0f, hab == 1 ? 1.0f : 0.0f, hab == 2 ? 1.0f : 0.0f, tile};
        u32 dispatch = 0;
        for (u32 l = 0; l < GC_LAYERS; l++) {
            i32 n = (i32)lroundf(tile / GC_LAYER_CELL[l]), m = (i32)ceilf(2.0f / GC_LAYER_CELL[l]);
            fc->gc_cell_x[l] = fc->gc_cell_z[l] = -m;
            fc->gc_cells[l] = (u32)(n + 2 * m);
            dispatch = MAX(dispatch, fc->gc_cells[l]);
        }
        VkImageView attach[2];
        for (u32 k = 0; k < 2; k++) {
            VkTex* t = &p->bake[hab][k];
            t->mips = mips;
            VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = formats[k];
            ci.extent = {size, size, 1};
            ci.mipLevels = mips;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &t->image));
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(vk->device, t->image, &req);
            t->memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
            VK_CHECK(vkBindImageMemory(vk->device, t->image, t->memory, 0));
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = t->image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = formats[k];
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
            VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &t->view));
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &attach[k]));
        }

        VkCommandBuffer cmd = vk_begin_once(vk);
        const VkPipelineStageFlags2 COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        for (u32 k = 0; k < 2; k++)
            vk_image_barrier(cmd, p->bake[hab][k].image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             COLOR_OUT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vk_image_barrier(cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        for (u32 k = 0; k < 2; k++) {
            color[k].imageView = attach[k];
            color[k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        color[1].clearValue.color = {{0.5f, 0.5f, 0.0f, 0.0f}};    // flat ground, height 0
        VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {size, size};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = color;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp = {0, 0, (f32)size, (f32)size, 0, 1};
        VkRect2D sc = {{0, 0}, {size, size}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipeline_layout, 0, 1, &vk->set, 0, NULL);
        vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(pcs), &pcs);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        // Several passes with other hashes: the dense lower sward under the taller tufts the meshes show (from above
        // a meadow shows little bare ground). The constants of pass k sit after the first FrameConstants block.
        for (u32 pass = 0; pass < GC_BAKE_PASSES; pass++) {
            FrameConstants* pf = (FrameConstants*)((u8*)fc + UPLOAD_NODES_OFFSET + pass * 1024);
            *pf = *fc;
            pf->gc_bake_pass = pass;
            PushConstants pp = pcs;
            pp.frame = pcs.frame + UPLOAD_NODES_OFFSET + pass * 1024;
            vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(pp), &pp);
            vkCmdDrawMeshTasksEXT(cmd, dispatch, dispatch, GC_LAYERS);
        }
        vkCmdEndRendering(cmd);

        // Mip chain by linear blits (sRGB-correct for the colour), then everything readable by shaders.
        for (u32 k = 0; k < 2; k++) {
            VkImage img = p->bake[hab][k].image;
            vk_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, COLOR_OUT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, mips - 1, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            for (u32 m = 1; m < mips; m++) {
                VkImageBlit b = {};
                b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
                b.srcOffsets[1] = {(i32)(size >> (m - 1)), (i32)(size >> (m - 1)), 1};
                b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                b.dstOffsets[1] = {(i32)(size >> m), (i32)(size >> m), 1};
                vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
                vk_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, m, 1, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            }
            vk_image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        vk_end_once(vk);
        for (u32 k = 0; k < 2; k++) {
            vkDestroyImageView(vk->device, attach[k], NULL);
            vk_bind_texture(vk, TEX_COVER + 2 * hab + k, p->bake[hab][k].view);
        }
    }
    vkDestroyImageView(vk->device, depth.view, NULL);
    vkDestroyImage(vk->device, depth.image, NULL);
    vkFreeMemory(vk->device, depth.memory, NULL);
    vkDestroyPipeline(vk->device, pipe, NULL);
}

// path: cooked file; shader_dir with trailing separator. Leaves p->enabled false if the file or mesh shaders are
// missing.
static void gc_init(GroundCover* p, Vk* vk, const char* path, const char* shader_dir)
{
    if (!vk->mesh_shaders) return;
    u64 t0 = SDL_GetPerformanceCounter();
    size_t size = 0;
    u8* file = (u8*)read_file(path, &size);
    if (!file) { printf("ground cover: %s not found (tools/fetch-polyhaven.sh + cook_ground_cover): ground cover off\n", path); return; }
    const GcFileHeader* h = (const GcFileHeader*)file;
    if (size < sizeof(*h) || h->magic != GC_FILE_MAGIC || h->version != GC_FILE_VERSION) FATAL("%s: not a cover file (or old version)", path);
    if (h->species_count > GC_MAX_SPECIES) FATAL("%s: too many species", path);
    const GcFileSpecies* fsp = (const GcFileSpecies*)(h + 1);
    const GcFileVariant* fvar = (const GcFileVariant*)(fsp + h->species_count);
    const TreeVertex* fverts = (const TreeVertex*)(fvar + h->variant_count);
    const u32* findex = (const u32*)(fverts + h->vertex_count);
    const u8* ftex = (const u8*)(findex + h->index_count);
    size_t tex_bytes = 0;
    for (u32 m = 0; m < h->tex_mips; m++) tex_bytes += (size_t)(h->tex_size >> m) * (h->tex_size >> m) * 4;
    if ((size_t)(ftex - file) + tex_bytes * 2 * h->species_count != size) FATAL("%s: size mismatch", path);

    // Variants x LODs -> meshlets (vertices compacted per mesh, see cook_ground_cover: every LOD has its own copy).
    GcVariant* variants = (GcVariant*)calloc(h->variant_count, sizeof(GcVariant));
    MeshBuild mb = {};
    MeshletArrays ma = {};
    i32* remap = (i32*)malloc(MAX(h->vertex_count, 1u) * sizeof(i32));
    for (u32 i = 0; i < h->vertex_count; i++) remap[i] = -1;
    u64 tris[GC_LODS] = {};
    for (u32 v = 0; v < h->variant_count; v++) {
        const GcFileVariant* fv = &fvar[v];
        GcVariant* pv = &variants[v];
        pv->species = fv->species;
        pv->height = fv->height;
        pv->radius = fv->radius;
        for (u32 lod = 0; lod < GC_LODS; lod++) {
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

    GcSpecies species[GC_MAX_SPECIES] = {};
    p->max_distance = 0;
    v3 grass_sum = {};
    f32 grass_weight = 0;
    for (u32 s = 0; s < h->species_count; s++) {
        GcSpecies* ps = &species[s];
        ps->first_variant = fsp[s].first_variant;
        ps->variant_count = fsp[s].variant_count;
        ps->albedo_tex = TEX_GC + 2 * s;
        ps->surface_tex = TEX_GC + 2 * s + 1;
        ps->habitat.w = fsp[s].has_alpha ? 1.0f : 0.0f;
        ps->shape = {1, 1, 0, 0};
        for (u32 r = 0; r < ARRAY_COUNT(GC_RULES); r++) {
            if (strcmp(GC_RULES[r].name, fsp[s].name)) continue;
            ps->habitat.x = GC_RULES[r].meadow;
            ps->habitat.y = GC_RULES[r].edge;
            ps->habitat.z = GC_RULES[r].floor;
            ps->shape = {GC_RULES[r].scale_min, GC_RULES[r].scale_max, GC_RULES[r].distance, GC_RULES[r].wind};
        }
        if (ps->shape.z == 0) printf("ground cover: no placement rule for %s, not placed\n", fsp[s].name);
        p->max_distance = MAX(p->max_distance, ps->shape.z);
        const u8* t = ftex + tex_bytes * 2 * s;
        // Meadow grasses (dense, short draw distance) follow the meadow height field and define the grass colour.
        if (ps->shape.z <= GC_NEAR_DISTANCE && ps->habitat.x >= 5.0f) {
            ps->habitat.w += 2.0f;
            // Mean over the living blades (opaque, green) of mip 2; the scans also contain bleached blades.
            const u8* m = t + (size_t)h->tex_size * h->tex_size * 4 + (size_t)(h->tex_size / 2) * (h->tex_size / 2) * 4;
            u32 ms = h->tex_size / 4;
            v3 sum = {};
            f32 n = 0;
            for (size_t i = 0; i < (size_t)ms * ms; i++) {
                const u8* x = m + 4 * i;
                if (x[3] < 128 || x[1] < x[0] * 1.15f || x[1] < x[2]) continue;
                for (u32 c = 0; c < 3; c++) {
                    f32 y = x[c] / 255.0f;
                    (&sum.x)[c] += y <= 0.04045f ? y / 12.92f : powf((y + 0.055f) / 1.055f, 2.4f);
                }
                n += 1;
            }
            if (n > 0) {
                grass_sum = v3_add(grass_sum, v3_scale(sum, ps->habitat.x / n));
                grass_weight += ps->habitat.x;
            }
        }
        p->tex[s][0] = vk_texture_2d_levels(vk, VK_FORMAT_R8G8B8A8_SRGB, 4, h->tex_size, h->tex_mips, t);
        p->tex[s][1] = vk_texture_2d_levels(vk, VK_FORMAT_R8G8B8A8_UNORM, 4, h->tex_size, h->tex_mips, t + tex_bytes);
        vk_bind_texture(vk, ps->albedo_tex, p->tex[s][0].view);
        vk_bind_texture(vk, ps->surface_tex, p->tex[s][1].view);
    }

    if (grass_weight > 0) {
        p->grass_color = v3_scale(grass_sum, 1.0f / grass_weight);
        printf("ground cover: meadow grass albedo %.3f %.3f %.3f\n", p->grass_color.x, p->grass_color.y, p->grass_color.z);
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    p->variant_buf = vk_buffer_static(vk, (VkDeviceSize)h->variant_count * sizeof(GcVariant), usage, variants);
    p->species_buf = vk_buffer_static(vk, sizeof(species), usage, species);
    p->meshlet_buf = vk_buffer_static(vk, (VkDeviceSize)ma.mn * sizeof(TreeMeshlet), usage, ma.m);
    p->vertex_buf = vk_buffer_static(vk, (VkDeviceSize)ma.vn * sizeof(TreeVertex), usage, ma.v);
    p->triangle_buf = vk_buffer_static(vk, (VkDeviceSize)ma.tn * sizeof(u32), usage, ma.t);
    GcScene scene = {p->variant_buf.address, p->species_buf.address, p->meshlet_buf.address, p->vertex_buf.address,
                        p->triangle_buf.address, h->species_count, 0};
    p->scene = vk_buffer_static(vk, sizeof(scene), usage, &scene);
    p->species_count = h->species_count;
    p->variant_count = h->variant_count;
    printf("ground cover: %u species, %u variants, triangles per LOD %llu / %llu / %llu, %u meshlets (%.1f MB), textures %u^2 (%.0f MB), %.0f ms\n",
           h->species_count, h->variant_count, (unsigned long long)tris[0], (unsigned long long)tris[1], (unsigned long long)tris[2], ma.mn,
           (ma.vn * sizeof(TreeVertex) + ma.tn * 4 + ma.mn * sizeof(TreeMeshlet)) / 1048576.0, h->tex_size,
           tex_bytes * 2 * h->species_count / 1048576.0, (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency());
    free(ma.v);
    free(ma.t);
    free(ma.m);
    free(variants);
    free(file);

    p->pipeline = veg_pipeline(vk, shader_dir, "gc");
    u64 t1 = SDL_GetPerformanceCounter();
    gc_bake(p, vk, shader_dir);
    printf("ground cover: top views of %u habitats baked (%u^2 per %.0f m tile), %.0f ms\n", GC_HABITATS, GC_BAKE_SIZE, GC_BAKE_TILE,
           (SDL_GetPerformanceCounter() - t1) * 1000.0 / SDL_GetPerformanceFrequency());
    p->enabled = true;
}

// Dispatch grids around the camera (terrain-local metres), one per layer; p->cells = dispatch side.
static void gc_frame(GroundCover* p, const f64 cam_local[3], FrameConstants* fc)
{
    f32 radius[GC_LAYERS] = {(f32)GC_NEAR_DISTANCE, p->max_distance};
    p->cells = 0;
    for (u32 l = 0; l < GC_LAYERS; l++) {
        u32 n = 2 * (u32)ceilf(radius[l] / GC_LAYER_CELL[l]) + 2;
        fc->gc_cells[l] = n;
        fc->gc_cell_x[l] = (i32)floor(cam_local[0] / GC_LAYER_CELL[l]) - (i32)(n / 2);
        fc->gc_cell_z[l] = (i32)floor(cam_local[2] / GC_LAYER_CELL[l]) - (i32)(n / 2);
        p->cells = MAX(p->cells, n);
    }
}