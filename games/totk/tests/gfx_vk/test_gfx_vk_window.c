/* test_gfx_vk.c with a real window: an SDL3 window with a Vulkan surface (what runtime/frontend_win.c provides through
   front_vk_instance_extensions / front_vk_surface), a swapchain, and the rendered target presented with mtl_present
   for a few frames, after the same pixel checks. Exit 77 without a desktop session. */
#include "nvn_vk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
static SDL_Window *g_win;
static void *win_surface(void *inst) {
    VkSurfaceKHR s = 0;
    if (!SDL_Vulkan_CreateSurface(g_win, (VkInstance)inst, NULL, &s)) return NULL;
    return (void *)(uintptr_t)s;
}
static int window_setup(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 0;
    g_win = SDL_CreateWindow("nvn-vk test", 640, 360, SDL_WINDOW_VULKAN);
    if (!g_win) return 0;
    Uint32 n = 0; const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!ext) return 0;
    nvn_vk_set_window(win_surface, ext, n);
    return 1;
}
static void window_pump(void) { SDL_Event e; while (SDL_PollEvent(&e)) {} }
#define GFX_VK_WINDOW 1
#include "test_gfx_vk.c"
