// Platform layer (SDL3): window, Vulkan surface, input mapping, main loop. Everything OS-specific lives here.

static const char* USAGE =
    "usage: vr [terrain.vrh] [options]\n"
    "  --cam E N ALT YAW PITCH   start camera (EPSG coordinates, altitude m, degrees; 'P' prints the current one)\n"
    "  --size WxH                window size (default 1920x1080)\n"
    "  --frames N                exit after N frames and print average timings\n"
    "  --shot FILE.ppm           save the last frame (with --frames)\n"
    "  --novsync                 present without vsync (for timing)\n"
    "  --validation              enable the Vulkan validation layer\n"
    "  --debug N                 start in view mode N (0 shaded, 1 LOD, 2 contours, 3 normals); --wire: wireframe\n"
    "\n"
    "controls: click = capture mouse, Esc = release (again = quit), WASD move, Q/E down/up (Space = up),\n"
    "  Shift x8, Ctrl x1/8, wheel = speed, G walk/fly, 1-4 shaded/LOD/contours/normals, L wireframe,\n"
    "  [ ] finer/coarser terrain LOD, V vsync, F12 screenshot, P print camera\n";

static ViewerCommand map_key(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_G: return CMD_TOGGLE_WALK;
    case SDL_SCANCODE_1: return CMD_DEBUG_SHADED;
    case SDL_SCANCODE_2: return CMD_DEBUG_LOD;
    case SDL_SCANCODE_3: return CMD_DEBUG_CONTOUR;
    case SDL_SCANCODE_4: return CMD_DEBUG_NORMALS;
    case SDL_SCANCODE_L: return CMD_TOGGLE_WIREFRAME;
    case SDL_SCANCODE_LEFTBRACKET: return CMD_LOD_FINER;
    case SDL_SCANCODE_RIGHTBRACKET: return CMD_LOD_COARSER;
    case SDL_SCANCODE_V: return CMD_TOGGLE_VSYNC;
    case SDL_SCANCODE_F12: return CMD_SCREENSHOT;
    case SDL_SCANCODE_P: return CMD_PRINT_CAMERA;
    default: return CMD_NONE;
    }
}

int main(int argc, char** argv)
{
    const char* terrain_path = "data/cooked/terrain.vrh";
    u32 width = 1920, height = 1080, max_frames = 0;
    const char* shot_path = NULL;
    bool validation = false, vsync = true, have_cam = false, wire = false;
    u32 debug_mode = 0;
    f64 cam_args[5] = {};
#ifdef VR_DEBUG
    validation = true;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cam") && i + 5 < argc) { for (int k = 0; k < 5; k++) cam_args[k] = atof(argv[++i]); have_cam = true; }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%ux%u", &width, &height) != 2) FATAL("--size WxH"); }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--novsync")) vsync = false;
        else if (!strcmp(argv[i], "--validation")) validation = true;
        else if (!strcmp(argv[i], "--debug") && i + 1 < argc) debug_mode = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wire")) wire = true;
        else if (argv[i][0] == '-') { fputs(USAGE, stderr); return 1; }
        else terrain_path = argv[i];
    }

    static Viewer v;   // large; keep off the stack
    terrain_load(&v.terrain, terrain_path);

    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s", SDL_GetError());
    SDL_Window* window = SDL_CreateWindow("vegetation-renderer", (int)width, (int)height,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) FATAL("SDL_CreateWindow: %s", SDL_GetError());
    Uint32 ext_count = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts) FATAL("SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
    vk_create_instance(&v.vk, exts, ext_count, validation);
    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, v.vk.instance, NULL, &surface)) FATAL("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    vk_init_device(&v.vk, surface);
    v.vk.vsync = vsync;
    int pw, ph;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    if (!vk_create_swapchain(&v.vk, (u32)pw, (u32)ph)) FATAL("window has zero size");

    char shader_path[1024];
    snprintf(shader_path, sizeof(shader_path), "%sshaders/terrain.spv", SDL_GetBasePath());
    terrain_upload(&v.terrain, &v.vk);
    terrain_create_pipelines(&v.terrain, &v.vk, shader_path);

    // Camera: --cam, else 60 m above the centre of the terrain looking north.
    const TerrainFileHeader* th = &v.terrain.hdr;
    Camera* cam = &v.cam;
    cam->speed = 15.0f;
    if (have_cam) {
        cam->pos[0] = cam_args[0] - th->origin_e;
        cam->pos[2] = cam_args[1] - th->origin_n;
        cam->pos[1] = cam_args[2];
        cam->yaw = (f32)(cam_args[3] * 3.14159265 / 180.0);
        cam->pitch = (f32)(cam_args[4] * 3.14159265 / 180.0);
    } else {
        cam->pos[0] = 0.5 * (th->width - 1) * th->spacing;
        cam->pos[2] = 0.5 * (th->height - 1) * th->spacing;
        cam->pos[1] = terrain_height(&v.terrain, cam->pos[0], cam->pos[2]) + 60.0;
        cam->pitch = -0.15f;
    }
    v.target_px = 6.0f;
    v.debug_mode = debug_mode;
    v.wireframe = wire && v.terrain.pipeline_wire;
    fputs(USAGE + strlen("usage: vr [terrain.vrh] [options]\n"), stdout);

    bool captured = false, running = true;
    u64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter(), title_time = last;
    u32 frames = 0, title_frames = 0;
    f64 sum_cpu_ms = 0, sum_gpu_ms = 0, title_cpu_ms = 0;
    while (running) {
        ViewerInput in = {};
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: v.swapchain_dirty = true; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (!captured) { captured = true; SDL_SetWindowRelativeMouseMode(window, true); }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (captured) { in.mouse_dx += e.motion.xrel; in.mouse_dy += e.motion.yrel; }
                break;
            case SDL_EVENT_MOUSE_WHEEL: in.wheel += e.wheel.y; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (captured) { captured = false; SDL_SetWindowRelativeMouseMode(window, false); }
                    else running = false;
                    break;
                }
                if (ViewerCommand c = map_key(e.key.scancode); c != CMD_NONE && in.command_count < ARRAY_COUNT(in.commands))
                    in.commands[in.command_count++] = c;
                break;
            }
        }
        // Scancodes: physical positions, so WASD works on QWERTZ/AZERTY layouts too.
        const bool* k = SDL_GetKeyboardState(NULL);
        in.forward = k[SDL_SCANCODE_W];
        in.back = k[SDL_SCANCODE_S];
        in.left = k[SDL_SCANCODE_A];
        in.right = k[SDL_SCANCODE_D];
        in.up = k[SDL_SCANCODE_E] || k[SDL_SCANCODE_SPACE];
        in.down = k[SDL_SCANCODE_Q];
        in.fast = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
        in.slow = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL];

        u64 now = SDL_GetPerformanceCounter();
        f32 dt = MIN((f32)((f64)(now - last) / freq), 0.1f);
        f64 frame_ms = (f64)(now - last) * 1000.0 / freq;
        last = now;
        viewer_update(&v, &in, dt);

        if (max_frames && frames + 1 == max_frames && shot_path) v.screenshot_path = shot_path;
        if (v.swapchain_dirty) {
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            if (!vk_create_swapchain(&v.vk, (u32)pw, (u32)ph)) { SDL_Delay(16); continue; }   // minimized
            v.swapchain_dirty = false;
        }
        if (!viewer_render(&v)) v.swapchain_dirty = true;

        frames++;
        if (frames > 1) { sum_cpu_ms += frame_ms; sum_gpu_ms += v.gpu_ms; }
        title_frames++;
        title_cpu_ms += frame_ms;
        if ((f64)(now - title_time) / freq > 0.25) {
            const Camera* c = &v.cam;
            char title[256];
            snprintf(title, sizeof(title), "vr | %.2f ms (gpu %.2f) | %u patches %.2f Mtri | E %.0f N %.0f alt %.1f (+%.1f) | %.1f m/s %s%s",
                     title_cpu_ms / title_frames, v.gpu_ms, v.node_count, v.node_count * TERRAIN_PATCH_QUADS * TERRAIN_PATCH_QUADS * 2 / 1e6,
                     th->origin_e + c->pos[0], th->origin_n + c->pos[2], c->pos[1], c->pos[1] - terrain_height(&v.terrain, c->pos[0], c->pos[2]),
                     c->speed, c->walk ? "walk" : "fly", captured ? "" : " | click to look");
            SDL_SetWindowTitle(window, title);
            title_time = now;
            title_frames = 0;
            title_cpu_ms = 0;
        }
        if (max_frames && frames >= max_frames) running = false;
    }
    vkDeviceWaitIdle(v.vk.device);
    if (max_frames) viewer_print_camera(&v);
    if (frames > 1)
        printf("%u frames: avg %.2f ms frame, %.2f ms gpu, %u patches\n", frames, sum_cpu_ms / (frames - 1), sum_gpu_ms / (frames - 1), v.node_count);
    // Process exit releases the rest (no per-object teardown; docs/02 lifetime rules).
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
