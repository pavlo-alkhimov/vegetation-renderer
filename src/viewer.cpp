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
} Camera;

#define CAMERA_EYE_HEIGHT 1.75f
#define CAMERA_MIN_CLEARANCE 0.5f
#define CAMERA_FOV_Y (60.0f * 3.14159265f / 180.0f)
#define CAMERA_NEAR 0.1f

typedef struct {
    Vk vk;
    Terrain terrain;
    Camera cam;
    u32 debug_mode;
    bool wireframe;
    f32 target_px;              // terrain triangle edge target (pixels)
    f32 time;
    // Stats
    u32 node_count;
    f32 gpu_ms;
    const char* screenshot_path;
    u32 screenshot_counter;
    bool swapchain_dirty;
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

static void viewer_update(Viewer* v, const ViewerInput* in, f32 dt)
{
    Camera* c = &v->cam;
    v->time += dt;
    for (u32 i = 0; i < in->command_count; i++) {
        switch (in->commands[i]) {
        case CMD_TOGGLE_WALK: c->walk = !c->walk; c->speed = c->walk ? 1.5f : 15.0f; break;
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
        case CMD_NONE: break;
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
    if (c->walk) c->pos[1] = ground + CAMERA_EYE_HEIGHT;
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

// Records and submits one frame. Returns false if the swapchain must be recreated.
static bool viewer_render(Viewer* v)
{
    Vk* vk = &v->vk;
    Terrain* t = &v->terrain;
    VkFrame* f = &vk->frames[vk->frame_index % FRAMES_IN_FLIGHT];
    VK_CHECK(vkWaitForFences(vk->device, 1, &f->fence, VK_TRUE, UINT64_MAX));
    if (f->submitted && vk->timestamps) {
        u64 ts[2];
        if (vkGetQueryPoolResults(vk->device, f->queries, 0, 2, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            v->gpu_ms = (f32)((ts[1] - ts[0]) * vk->props.limits.timestampPeriod * 1e-6);
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
    fc->height_tex = 0;
    fc->debug_mode = v->debug_mode;
    terrain_set_ranges(t, proj_y * vk->extent.height * 0.5f, v->target_px, fc);

    const VkDeviceSize nodes_offset = 1024;
    static_assert(sizeof(FrameConstants) <= 1024, "FrameConstants too large");
    static_assert(1024 + TERRAIN_MAX_NODES * sizeof(TerrainNode) <= UPLOAD_BYTES, "upload buffer too small");
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
    terrain_select_node(t, &sel, t->levels - 1, 0, 0);
    v->node_count = sel.count;

    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandPool(vk->device, f->pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    if (vk->timestamps) {
        vkCmdResetQueryPool(cmd, f->queries, 0, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, f->queries, 0);
    }
    vk_image_barrier(cmd, vk->images[image], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_image_barrier(cmd, vk->depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = vk->views[image];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // sky covers every pixel the terrain does not
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = vk->depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil.depth = 0.0f;       // reversed Z
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (f32)vk->extent.width, (f32)vk->extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, vk->extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipeline_layout, 0, 1, &vk->set, 0, NULL);
    PushConstants pcs = {f->upload.address, f->upload.address + nodes_offset};
    vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(pcs), &pcs);
    if (sel.count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->wireframe ? t->pipeline_wire : t->pipeline);
        vkCmdBindIndexBuffer(cmd, t->indices.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, t->index_count, sel.count, 0, 0, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t->pipeline_sky);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (vk->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->queries, 1);

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
