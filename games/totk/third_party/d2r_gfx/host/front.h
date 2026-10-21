#pragma once
#include <stdint.h>
void *front_metal_layer(void);
void front_frame_presented(void);
uint64_t front_game_res(int docked);
#ifdef _WIN32
/* Windows frontend (runtime/frontend_win.c, SDL3; docs/windows-port-plan.md C9): what the Vulkan backend (C11) needs
   in place of front_metal_layer(). Handles are passed as void * / uint64_t so this header needs no Vulkan headers. */
/* Instance extensions the window's surface needs (VK_KHR_surface, VK_KHR_win32_surface); NULL with *count 0 before the
   window exists or without a Vulkan loader. The array stays valid for the life of the process. */
const char *const *front_vk_instance_extensions(uint32_t *count);
/* A VkSurfaceKHR (as uint64_t) for the game window on vk_instance (a VkInstance); 0 on failure. Any thread; the caller
   owns the surface (vkDestroySurfaceKHR). */
uint64_t front_vk_surface(void *vk_instance);
/* The window's drawable size in pixels (follows resizes and DPI changes; the swapchain extent). 0 before the window. */
int front_drawable_size(uint32_t *w, uint32_t *h);
void *front_sdl_window(void);   /* SDL_Window *, NULL before the window */
void *front_win32_hwnd(void);   /* HWND, NULL before the window */
#endif
