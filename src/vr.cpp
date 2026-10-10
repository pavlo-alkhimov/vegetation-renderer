// vr: viewer executable, unity build (one translation unit).
#include "base.h"
#include "gpu_shared.h"
#include "terrain_file.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include "vk_functions.cpp"
#include "vk.cpp"
#include "terrain.cpp"
#include "viewer.cpp"
#include "platform_sdl.cpp"
