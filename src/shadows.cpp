// Sun shadows: SHADOW_CASCADES orthographic cascades in one D32 array (reversed Z), rendered with the vegetation
// task/mesh shaders through depth-only pipelines (views[1 + c] in the upload buffer). Casters: trees in all cascades
// (one LOD coarser in cascades 0-1, two in 2, billboards in 3), cover only in cascade 0 (further out their
// self-shadowing is left to AO). The terrain does not cast yet (its n.l term covers the large-scale shading; hills
// behind the camera would need their own node selection).

#define SHADOW_BACK 300.0f              // m: casters this far towards the sun from a cascade's centre still count
static const f32 SHADOW_SPLIT[SHADOW_CASCADES] = {6.0f, 25.0f, 100.0f, 400.0f};    // view depth (m) of each far end

typedef struct {
    bool enabled;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView array_view;
    VkImageView layer_view[SHADOW_CASCADES];
    VkPipeline trees, cover;
    // Per frame
    u32 chunk_first, chunk_count;       // tree chunks for the shadow passes (after the main view's chunks)
    u32 update;                         // bit c: cascade c is rendered this frame
    // Cascades 2-3 are rendered on alternate frames; the others keep their last view (valid while the render
    // origin and the sun stay).
    ViewConstants last[SHADOW_CASCADES];
    f32 last_texel[SHADOW_CASCADES];
    f64 last_origin[3];
    v3 last_sun;
    u64 frame;
    bool valid;
} Shadows;

static void shadows_init(Shadows* s, Vk* vk, const char* shader_dir)
{
    if (!vk->mesh_shaders) return;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = DEPTH_FORMAT;
    ci.extent = {SHADOW_SIZE, SHADOW_SIZE, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = SHADOW_CASCADES;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &s->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, s->image, &req);
    s->memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
    VK_CHECK(vkBindImageMemory(vk->device, s->image, s->memory, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = DEPTH_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, SHADOW_CASCADES};
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &s->array_view));
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    for (u32 c = 0; c < SHADOW_CASCADES; c++) {
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &s->layer_view[c]));
    }
    VkCommandBuffer cmd = vk_begin_once(vk);
    vk_image_barrier(cmd, s->image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_end_once(vk);
    // vk_image_barrier covers one layer; transition the rest the same way.
    for (u32 c = 1; c < SHADOW_CASCADES; c++) {
        cmd = vk_begin_once(vk);
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        vk_end_once(vk);
    }

    VkDescriptorImageInfo ii = {};
    ii.imageView = s->array_view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = vk->set;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);

    s->trees = veg_pipeline(vk, shader_dir, "trees", true);
    s->cover = veg_pipeline(vk, shader_dir, "gc", true);
    s->enabled = true;
}

// Orthographic view of one cascade. Fitted to a bounding sphere of the camera frustum slice [n, f], whose radius
// does not change when the camera turns; the centre is snapped to whole texels, so the map does not shimmer.
static void shadow_cascade(ViewConstants* vc, v3 cam, v3 fwd, f32 px, f32 py, f32 n, f32 f, v3 l, f32 lod_bias, f32* texel)
{
    f32 t2 = 1.0f / (px * px) + 1.0f / (py * py);       // tan^2 of the corner angle
    f32 d = MIN(0.5f * (1.0f + t2) * (f + n), f);
    f32 r = sqrtf((f - d) * (f - d) + f * f * t2);
    r = ceilf(r * 16.0f) / 16.0f;
    v3 c = v3_add(cam, v3_scale(fwd, d));
    v3 x = v3_norm(v3_cross(fabsf(l.y) > 0.99f ? v3_make(1, 0, 0) : v3_make(0, 1, 0), l));
    v3 y = v3_cross(l, x);
    *texel = 2.0f * r / SHADOW_SIZE;
    f32 cx = v3_dot(x, c), cy = v3_dot(y, c);
    c = v3_add(c, v3_add(v3_scale(x, floorf(cx / *texel) * *texel - cx), v3_scale(y, floorf(cy / *texel) * *texel - cy)));
    // clip.x/y in [-1, 1] across the sphere; depth 1 at SHADOW_BACK towards the sun, 0 at r away from it.
    f32 depth_scale = 1.0f / (SHADOW_BACK + r);
    vc->view_proj[0] = {x.x / r, x.y / r, x.z / r, -v3_dot(x, c) / r};
    vc->view_proj[1] = {y.x / r, y.y / r, y.z / r, -v3_dot(y, c) / r};
    vc->view_proj[2] = {l.x * depth_scale, l.y * depth_scale, l.z * depth_scale, 1.0f - (v3_dot(l, c) + SHADOW_BACK) * depth_scale};
    vc->view_proj[3] = {0, 0, 0, 1};
    memcpy(vc->prev_view_proj, vc->view_proj, sizeof(vc->view_proj));
    vc->planes[0] = {x.x, x.y, x.z, -v3_dot(x, c) + r};
    vc->planes[1] = {-x.x, -x.y, -x.z, v3_dot(x, c) + r};
    vc->planes[2] = {y.x, y.y, y.z, -v3_dot(y, c) + r};
    vc->planes[3] = {-y.x, -y.y, -y.z, v3_dot(y, c) + r};
    vc->planes[4] = {l.x, l.y, l.z, -v3_dot(l, c) + r};               // not beyond r away from the sun
    vc->planes[5] = {-l.x, -l.y, -l.z, v3_dot(l, c) + SHADOW_BACK};   // not beyond SHADOW_BACK towards it
    vc->jitter = {0, 0, lod_bias, 0};
}

// Fills views[1..] and the FrameConstants shadow fields; selects tree chunks around the camera (no frustum test:
// casters behind the camera shade the view) after the main view's chunks.
static void shadows_frame(Shadows* s, Vegetation* vg, ViewConstants* views, FrameConstants* fc, v3 cam, v3 fwd, f32 px, f32 py, f32 near,
                          const f64 origin[3], const f64 cam_local[3], TreeChunk* chunks, bool trees)
{
    v3 l = v3_make(fc->sun_dir.x, fc->sun_dir.y, fc->sun_dir.z);
    bool all = !s->valid || memcmp(s->last_origin, origin, sizeof(s->last_origin)) || memcmp(&s->last_sun, &l, sizeof(l));
    f32 n = near;
    s->update = 0;
    for (u32 c = 0; c < SHADOW_CASCADES; c++) {
        if (all || c < 2 || (s->frame & 1) == c - 2) {
            shadow_cascade(&s->last[c], cam, fwd, px, py, n, SHADOW_SPLIT[c], l, c < 2 ? 1.0f : (f32)c, &s->last_texel[c]);
            s->update |= 1u << c;
        }
        views[1 + c] = s->last[c];
        (&fc->shadow_texel.x)[c] = s->last_texel[c];
        (&fc->shadow_split.x)[c] = SHADOW_SPLIT[c];
        n = SHADOW_SPLIT[c];
    }
    memcpy(s->last_origin, origin, sizeof(s->last_origin));
    s->last_sun = l;
    s->valid = true;
    s->frame++;
    fc->shadows = 1;
    s->chunk_first = vg->chunk_count;
    s->chunk_count = 0;
    if (!trees) return;
    const f32 radius = SHADOW_SPLIT[SHADOW_CASCADES - 1] + 30.0f;
    i32 cx0 = MAX(0, (i32)floor((cam_local[0] - radius) / TREE_CELL)), cx1 = MIN((i32)vg->cells_x - 1, (i32)floor((cam_local[0] + radius) / TREE_CELL));
    i32 cz0 = MAX(0, (i32)floor((cam_local[2] - radius) / TREE_CELL)), cz1 = MIN((i32)vg->cells_z - 1, (i32)floor((cam_local[2] + radius) / TREE_CELL));
    for (i32 cz = cz0; cz <= cz1; cz++)
        for (i32 cx = cx0; cx <= cx1; cx++) {
            const TreeCell* cell = &vg->cells[cz * vg->cells_x + cx];
            f32 ox = (f32)(cx * TREE_CELL - origin[0]), oz = (f32)(cz * TREE_CELL - origin[2]);
            for (u32 k = 0; k < cell->count && s->chunk_first + s->chunk_count < MAX_TREE_CHUNKS; k += TREE_CHUNK)
                chunks[s->chunk_first + s->chunk_count++] = {cell->first + k, MIN((u32)TREE_CHUNK, cell->count - k), ox, oz};
        }
}

// Records the cascades. pcs: the main view's push constants (view and chunk pointers are changed per pass).
static void shadows_record(Shadows* s, Vk* vk, VkCommandBuffer cmd, PushConstants pcs, VkDeviceAddress views, VkDeviceAddress chunks,
                           const GroundCover* p, bool trees, bool cover)
{
    const VkPipelineStageFlags2 FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = FS;
    b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, SHADOW_CASCADES};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);

    VkViewport vp = {0, 0, (f32)SHADOW_SIZE, (f32)SHADOW_SIZE, 0, 1};
    VkRect2D sc = {{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
    for (u32 c = 0; c < SHADOW_CASCADES; c++) {
        if (!(s->update & (1u << c))) continue;
        VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = s->layer_view[c];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil.depth = 0.0f;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {SHADOW_SIZE, SHADOW_SIZE};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        pcs.view = views + (1 + c) * sizeof(ViewConstants);
        pcs.chunks = chunks + s->chunk_first * sizeof(TreeChunk);
        vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(pcs), &pcs);
        if (trees && s->chunk_count) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->trees);
            vkCmdDrawMeshTasksEXT(cmd, s->chunk_count, 1, 1);
        }
        if (c == 0 && cover) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->cover);
            vkCmdDrawMeshTasksEXT(cmd, p->cells, p->cells, GC_LAYERS);
        }
        vkCmdEndRendering(cmd);
    }
    b.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = FS;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &di);
}
