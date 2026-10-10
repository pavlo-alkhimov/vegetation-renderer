// Platform layer (SDL3): window, Vulkan surface, input mapping, main loop. Everything OS-specific lives here.

static const char* USAGE =
    "usage: vr [terrain.vrh] [options]\n"
    "  --cam E N ALT YAW PITCH   start camera (EPSG coordinates, altitude m, degrees; 'P' prints the current one)\n"
    "  --size WxH                window size (default 1920x1080)\n"
    "  --frames N                exit after N frames and print average timings\n"
    "  --shot FILE.ppm           save the last frame (with --frames)\n"
    "  --novsync                 present without vsync (for timing)\n"
    "  --hidpi                   render at native pixel density (Retina / scaled displays; default: 1 pixel per point)\n"
    "  --validation              enable the Vulkan validation layer\n"
    "  --debug N                 start in view mode N (0 shaded, 1 LOD, 2 contours, 3 normals); --wire: wireframe\n"
    "  --notrees, --nograss      start with trees / grass off (A/B timing)\n"
    "  --treedist M              tree draw distance in m (default 3000); --grass M: grass radius (default 60)\n"
    "\n"
    "controls: click = capture mouse, Esc = release (again = quit), WASD move, Q/E down/up (Space = up),\n"
    "  Shift x8, Ctrl x1/8, wheel = speed, G walk/fly, 1-4 shaded/LOD/contours/normals, L wireframe,\n"
    "  [ ] finer/coarser terrain LOD, T trees, B grass, - = tree distance, V vsync, F12 or K screenshot,\n"
    "  P print camera, H overlay (FPS graph, keys)\n";

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
    case SDL_SCANCODE_K: return CMD_SCREENSHOT;    // laptops: F12 needs fn (macOS)
    case SDL_SCANCODE_P: return CMD_PRINT_CAMERA;
    case SDL_SCANCODE_T: return CMD_TOGGLE_TREES;
    case SDL_SCANCODE_B: return CMD_TOGGLE_GRASS;
    case SDL_SCANCODE_MINUS: return CMD_TREE_DIST_LESS;
    case SDL_SCANCODE_EQUALS: return CMD_TREE_DIST_MORE;
    case SDL_SCANCODE_H: return CMD_TOGGLE_OVERLAY;
    default: return CMD_NONE;
    }
}

int main(int argc, char** argv)
{
    const char* terrain_path = "data/cooked/terrain.vrh";
    u32 width = 1920, height = 1080, max_frames = 0;
    const char* shot_path = NULL;
    bool validation = false, vsync = true, have_cam = false, wire = false, hidpi = false, trees = true, grass = true;
    u32 debug_mode = 0;
    f32 tree_dist = 3000.0f, grass_radius = 60.0f;
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
        else if (!strcmp(argv[i], "--hidpi")) hidpi = true;
        else if (!strcmp(argv[i], "--validation")) validation = true;
        else if (!strcmp(argv[i], "--debug") && i + 1 < argc) debug_mode = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wire")) wire = true;
        else if (!strcmp(argv[i], "--notrees")) trees = false;
        else if (!strcmp(argv[i], "--nograss")) grass = false;
        else if (!strcmp(argv[i], "--treedist") && i + 1 < argc) tree_dist = (f32)atof(argv[++i]);
        else if (!strcmp(argv[i], "--grass") && i + 1 < argc) grass_radius = (f32)atof(argv[++i]);
        else if (argv[i][0] == '-') { fputs(USAGE, stderr); return 1; }
        else terrain_path = argv[i];
    }

    static Viewer v;   // large; keep off the stack
    terrain_load(&v.terrain, terrain_path);

    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s", SDL_GetError());

    // One Vulkan loader for SDL and us (vk_functions.cpp). SDL's default search covers Linux, Windows and a
    // system-wide macOS SDK install; the fallbacks cover a non-global macOS SDK and Homebrew.
    bool vulkan_loaded = SDL_Vulkan_LoadLibrary(NULL);
#ifdef __APPLE__
    char sdk_path[1024];
    const char* fallbacks[] = {sdk_path, "/usr/local/lib/libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib"};
    snprintf(sdk_path, sizeof(sdk_path), "%s/lib/libvulkan.1.dylib", getenv("VULKAN_SDK") ? getenv("VULKAN_SDK") : ".");
    for (u32 i = 0; i < ARRAY_COUNT(fallbacks) && !vulkan_loaded; i++) vulkan_loaded = SDL_Vulkan_LoadLibrary(fallbacks[i]);
#endif
    if (!vulkan_loaded) FATAL("cannot load the Vulkan loader: %s", SDL_GetError());
    vk_load_global((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr());

    SDL_Window* window = SDL_CreateWindow("vegetation-renderer", (int)width, (int)height,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (hidpi ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0));
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
    snprintf(shader_path, sizeof(shader_path), "%sshaders/", SDL_GetBasePath());
    veg_init(&v.veg, &v.terrain, &v.vk, shader_path);
    overlay_init(&v.overlay, &v.vk, shader_path);
    v.show_trees = trees;
    v.show_grass = grass;
    v.tree_dist = tree_dist;
    v.grass_radius = grass_radius;

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
    f64 sum_cpu_ms = 0, sum_gpu_ms = 0, title_cpu_ms = 0, sum_pass_ms[3] = {};
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
        overlay_frame(&v.overlay, (f32)(frame_ms * 0.001));
        viewer_update(&v, &in, dt);

        if (max_frames && frames + 1 == max_frames && shot_path) v.screenshot_path = shot_path;
        if (v.swapchain_dirty) {
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            if (!vk_create_swapchain(&v.vk, (u32)pw, (u32)ph)) { SDL_Delay(16); continue; }   // minimized
            v.swapchain_dirty = false;
        }
        if (!viewer_render(&v)) v.swapchain_dirty = true;

        frames++;
        if (frames > 1) {
            sum_cpu_ms += frame_ms;
            sum_gpu_ms += v.gpu_ms;
            for (u32 i = 0; i < 3; i++) sum_pass_ms[i] += v.gpu_pass_ms[i];
        }
        title_frames++;
        title_cpu_ms += frame_ms;
        if ((f64)(now - title_time) / freq > 0.25) {
            const Camera* c = &v.cam;
            char title[384];
            snprintf(title, sizeof(title), "vr %ux%u | %.2f ms, gpu %.2f (terrain %.2f trees %.2f grass %.2f) | %u trees in %u chunks, dist %.0f m | "
                     "E %.0f N %.0f alt %.1f (+%.1f) | %.1f m/s %s%s",
                     v.vk.extent.width, v.vk.extent.height, title_cpu_ms / title_frames, v.gpu_ms, v.gpu_pass_ms[0], v.gpu_pass_ms[1], v.gpu_pass_ms[2],
                     v.veg.tree_candidates, v.veg.chunk_count, v.tree_dist,
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
        printf("%u frames at %ux%u: avg %.2f ms frame, %.2f ms gpu (terrain %.2f, trees %.2f, grass %.2f), %u patches, %u tree candidates\n",
               frames, v.vk.extent.width, v.vk.extent.height, sum_cpu_ms / (frames - 1), sum_gpu_ms / (frames - 1),
               sum_pass_ms[0] / (frames - 1), sum_pass_ms[1] / (frames - 1), sum_pass_ms[2] / (frames - 1), v.node_count, v.veg.tree_candidates);
    // Process exit releases the rest (no per-object teardown; docs/02 lifetime rules).
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
