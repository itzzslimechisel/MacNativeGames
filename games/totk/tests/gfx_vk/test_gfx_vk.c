/* Vulkan NVN backend, end to end through the mtl_* seam (docs/windows-port-plan.md C11 / C20), no game data:
   - memory pools in guest memory (mirror mode with dirty uploads + readback, or TOTK_VK_POOL=host);
   - a 4x4 RGBA8 block-linear texture in a CPU-visible pool (deswizzled, uploaded, re-uploaded after a CPU write);
   - an ASTC 4x4 texture (CPU-decoded when the GPU has no ASTC: NVIDIA / AMD / Intel desktop);
   - hand-assembled Maxwell shaders translated by totk_xlate --target spirv (fixtures/test_{vs,ps}_v.{spv,json}):
     a full-screen triangle whose fragment shader samples combined texture slot 0;
   - NVN command records (nvn_obj.h) replayed by mtl_exec: set targets, clear, bind program / vertex state / texture,
     draw, copy texture to buffer, buffer to buffer copy, signal event; then the CPU checks the pool bytes.
   Exit 0 = pass, 1 = fail, 77 = no Vulkan device. */
#include "nvn_vk.h"
#include "nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static const char *g_fix;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t g_ctl_vs[0x880], g_ctl_ps[0x880];
static uint8_t g_code_vs[64], g_code_ps[64];
static int resolve(int stage, const uint8_t *control, const uint8_t *code, char *out, size_t n) {
    (void)code;
    if (control == g_ctl_vs && stage == 0) { snprintf(out, n, "%s/test_vs_v", g_fix); return 1; }
    if (control == g_ctl_ps && stage == 1) { snprintf(out, n, "%s/test_ps_v", g_fix); return 1; }
    return 0;
}

static uint8_t *pool_alloc(size_t n) {
#ifdef _WIN32
    return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p; posix_memalign(&p, 65536, n); memset(p, 0, n); return p;
#endif
}

// ---------- command record writer ----------
static uint8_t g_cmd[1 << 16]; static size_t g_ncmd;
static void put(uint32_t op, const void *payload, size_t n) {
    op_hdr h = {op, (uint32_t)((8 + n + 7) & ~(size_t)7)};
    memcpy(g_cmd + g_ncmd, &h, 8); memset(g_cmd + g_ncmd + 8, 0, h.size - 8); if (n) memcpy(g_cmd + g_ncmd + 8, payload, n);
    g_ncmd += h.size;
}
static void put_state(int kind, const void *st, size_t n) { p_bind_state b = {kind, {0}}; memcpy(b.bytes, st, n); put(OP_BIND_STATE, &b, sizeof b); }
static uint64_t run(void) {
    put(OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(g_cmd); uint64_t f = mtl_sub_commit();
    g_ncmd = 0;
    CHECK(mtl_wait(f, 5000000000ull), "GPU did not finish fence %llu", (unsigned long long)f);
    return f;
}

static void tex_init(tex_t *t, uint64_t serial, int format, uint32_t w, uint32_t h, int flags, uint8_t *mem, pool_t *pool) {
    memset(t, 0, sizeof *t);
    t->serial = serial; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.format = format; t->d.width = w; t->d.height = h; t->d.depth = 1; t->d.levels = 1;
    t->d.flags = flags; t->d.swizzle_r = 2; t->d.swizzle_g = 3; t->d.swizzle_b = 4; t->d.swizzle_a = 5;
    nvn_tex_layout(&t->d, &t->L); t->mem = mem; t->pool = pool;
}

int main(int argc, char **argv) {
    g_fix = argc > 1 ? argv[1] : "fixtures";
#ifdef GFX_VK_WINDOW
    if (!window_setup()) { printf("SKIP: no window / Vulkan surface\n"); return 77; }
#endif
    mtl_init();
    if (!nvn_vk_available()) { printf("SKIP: no Vulkan 1.3 device\n"); return 77; }
    printf("device: %s, pool mode %d\n", nvn_vk_device_name(), nvn_vk_pool_mode());
    nvn_vk_set_resolver(resolve);

    // pools: a CPU-visible one (vertices, textures, readback targets) and a GPU-only one (render target storage)
    const size_t PSZ = 4 << 20;
    uint8_t *cpu = pool_alloc(PSZ), *gpu = pool_alloc(PSZ);
    pool_t pc = {0, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, cpu, PSZ, NULL, NULL, 0};
    pool_t pg = {0, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, gpu, PSZ, NULL, NULL, 0};
    mtl_pool_add(cpu, PSZ); mtl_pool_add(gpu, PSZ);

    // vertex buffer at cpu+0: full-screen triangle, position (y up) + uv (texture row 0 at the top)
    float vtx[3][8] = {{-1, -1, 0, 1, 0, 1, 0, 0}, {3, -1, 0, 1, 2, 1, 0, 0}, {-1, 3, 0, 1, 0, -1, 0, 0}};
    memcpy(cpu, vtx, sizeof vtx);
    // 4x4 RGBA8 texture at cpu+64K: quadrants red / green / blue / white
    static const uint32_t Q[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
    uint32_t lin[16]; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) lin[y * 4 + x] = Q[(y / 2) * 2 + x / 2];
    tex_t tex; tex_init(&tex, 101, 0x25, 4, 4, 0, cpu + 0x10000, &pc);
    CHECK(!nvn_swizzle(&tex.d, 0, 0, lin, 16, tex.mem), "swizzle");
    // ASTC 4x4 void-extent block (constant colour 0x40, 0x80, 0xC0, 0xFF) at cpu+128K
    tex_t astc; tex_init(&astc, 102, 0x79, 4, 4, 0, cpu + 0x20000, &pc);
    { uint8_t blk[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; uint16_t c[4] = {0x4040, 0x8080, 0xC0C0, 0xFFFF}; memcpy(blk + 8, c, 8);
      CHECK(!nvn_swizzle(&astc.d, 0, 0, blk, 16, astc.mem), "swizzle astc"); }
    // render target 64x64 RGBA8 in the GPU pool
    tex_t rt; tex_init(&rt, 103, 0x25, 64, 64, 0, gpu, &pg);
    mtl_reg_tex(1, &tex, NULL); mtl_reg_tex(2, &astc, NULL);
    samp_par sp = {NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, 0, 0, 0, 0, 0, 0, 1, {0}, 0};
    mtl_reg_samp(1, &sp);

    // program: vertex + fragment stages (control +0x704 code length, +0x714 NVN stage)
    uint32_t len = 64, st0 = 0, st1 = 1;
    memcpy(g_ctl_vs + 0x704, &len, 4); memcpy(g_ctl_vs + 0x714, &st0, 4); memcpy(g_ctl_ps + 0x704, &len, 4); memcpy(g_ctl_ps + 0x714, &st1, 4);
    prog_t prog = {0, 2, {{(NVNbufferAddress)(uintptr_t)g_code_vs, g_ctl_vs}, {(NVNbufferAddress)(uintptr_t)g_code_ps, g_ctl_ps}}, 201};

    volatile uint32_t ev = 0;
    uint8_t *out = cpu + 0x100000;          // texture-to-buffer readback target (64x64x4 = 16 KiB)
    uint8_t *cpy = cpu + 0x110000;          // buffer-to-buffer copy target

    // ---- submission 1: clear, draw the textured triangle, copy the target to the pool, copy a buffer, signal
    p_set_rt srt; memset(&srt, 0, sizeof srt); srt.n = 1; srt.color[0] = &rt; put(OP_SET_RT, &srt, sizeof srt);
    p_clear_color cc = {0, 0xf, {0.25f, 0.5f, 0.75f, 1.0f}}; put(OP_CLEAR_COLOR, &cc, sizeof cc);
    p_bind_program bp = {&prog, 3}; put(OP_BIND_PROGRAM, &bp, sizeof bp);
    va_st a0 = {0x2E, 0, 0}, a1 = {0x2E, 16, 0}; p_bind_vattr va = {0, 2, {0}}; memcpy(&va.st[0], &a0, 4); memcpy(&va.st[1], &a1, 4); put(OP_BIND_VATTR, &va, sizeof va);
    p_bind_vstream vsr = {0, 1, {{32, 0}}}; put(OP_BIND_VSTREAM, &vsr, sizeof vsr);
    p_bind_vbuf vb = {0, 1, {{(NVNbufferAddress)(uintptr_t)cpu, sizeof vtx}}}; put(OP_BIND_VBUF, &vb, sizeof vb);
    p_bind_handles th; memset(&th, 0, sizeof th); th.stage = 1; th.first = 0; th.n = 1; th.h[0] = 1 | (1ull << 20); put(OP_BIND_TEX, &th, sizeof th);
    p_viewports vp; memset(&vp, 0, sizeof vp); vp.first = 0; vp.n = 1; vp.v[0][2] = 64; vp.v[0][3] = 64; vp.v[0][5] = 1; put(OP_VIEWPORTS, &vp, sizeof vp);
    poly_st poly = {NVN_FACE_BACK, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; put_state(4, &poly, sizeof poly);
    p_draw dw = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 0, 0}; put(OP_DRAW, &dw, sizeof dw);
    p_copy_stride cs = {64 * 4}; put(OP_COPY_STRIDE, &cs, sizeof cs);
    p_copy_b2t t2b; memset(&t2b, 0, sizeof t2b); t2b.src = (uint64_t)(uintptr_t)out; t2b.tex = &rt; t2b.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1}; put(OP_COPY_T2B, &t2b, sizeof t2b);
    uint64_t b2b[3] = {(uint64_t)(uintptr_t)(cpu + 0x10000), (uint64_t)(uintptr_t)cpy, 64}; put(OP_COPY_B2B, b2b, sizeof b2b);
    struct { volatile uint32_t *v; uint32_t mode, val; } se = {&ev, 0, 7}; put(OP_SIGNAL_EVENT, &se, sizeof se);
    run();
    CHECK(ev == 7, "event value %u (want 7)", ev);
    uint32_t px[64 * 64]; memcpy(px, out, sizeof px);
    // quadrant centres: top-left red, top-right green, bottom-left blue, bottom-right white (texture row 0 at the top)
    CHECK(px[16 * 64 + 16] == Q[0], "TL %08x", px[16 * 64 + 16]);
    CHECK(px[16 * 64 + 48] == Q[1], "TR %08x", px[16 * 64 + 48]);
    CHECK(px[48 * 64 + 16] == Q[2], "BL %08x", px[48 * 64 + 16]);
    CHECK(px[48 * 64 + 48] == Q[3], "BR %08x", px[48 * 64 + 48]);
    CHECK(!memcmp(cpy, cpu + 0x10000, 64), "buffer copy readback");
    printf("draw 1: TL %08x TR %08x BL %08x BR %08x\n", px[16 * 64 + 16], px[16 * 64 + 48], px[48 * 64 + 16], px[48 * 64 + 48]);

    // ---- submission 2: the CPU rewrites the texture's pool bytes (dirty upload), same draw again
    for (int i = 0; i < 16; i++) lin[i] = 0xff00ffff;   // yellow
    nvn_swizzle(&tex.d, 0, 0, lin, 16, tex.mem);
    put(OP_SET_RT, &srt, sizeof srt); put(OP_BIND_PROGRAM, &bp, sizeof bp); put(OP_DRAW, &dw, sizeof dw); put(OP_COPY_T2B, &t2b, sizeof t2b);
    run();
    memcpy(px, out, sizeof px);
    CHECK(px[16 * 64 + 16] == 0xff00ffff && px[48 * 64 + 48] == 0xff00ffff, "re-upload after CPU write: %08x %08x", px[16 * 64 + 16], px[48 * 64 + 48]);

    // ---- submission 3: ASTC texture (CPU-decoded on desktop GPUs), drawn the same way
    th.h[0] = 2 | (1ull << 20);
    put(OP_SET_RT, &srt, sizeof srt); put(OP_BIND_TEX, &th, sizeof th); put(OP_DRAW, &dw, sizeof dw); put(OP_COPY_T2B, &t2b, sizeof t2b);
    run();
    memcpy(px, out, sizeof px);
    CHECK(px[32 * 64 + 32] == 0xffc08040, "ASTC void-extent colour %08x (want ffc08040)", px[32 * 64 + 32]);

    // ---- submission 4: winding: front face CW with back-face culling removes the (CCW) triangle
    cc.rgba[0] = 0; cc.rgba[1] = 0; cc.rgba[2] = 0; cc.rgba[3] = 1;
    poly.front = NVN_FRONT_FACE_CW;
    put(OP_SET_RT, &srt, sizeof srt); put(OP_CLEAR_COLOR, &cc, sizeof cc); put_state(4, &poly, sizeof poly); put(OP_DRAW, &dw, sizeof dw); put(OP_COPY_T2B, &t2b, sizeof t2b);
    run();
    memcpy(px, out, sizeof px);
    CHECK(px[32 * 64 + 32] == 0xff000000, "culled triangle still drawn: %08x", px[32 * 64 + 32]);

    // ---- CPU texel read of the render target (ReadTexels path)
    uint32_t rb[4 * 4]; NVNcopyRegion r4 = {30, 30, 0, 4, 4, 1};
    mtl_read_texels(&rt, 0, 0, &r4, (uint8_t *)rb, 16, 64);
    CHECK(rb[0] == 0xff000000, "read texels %08x", rb[0]);

#ifdef GFX_VK_WINDOW
    // present the first draw's picture (re-drawn with the yellow texture: submission 2 state) for 60 frames
    poly.front = NVN_FRONT_FACE_CCW; th.h[0] = 1 | (1ull << 20);
    for (int f = 0; f < 60; f++) {
        put(OP_SET_RT, &srt, sizeof srt); put_state(4, &poly, sizeof poly); put(OP_BIND_TEX, &th, sizeof th);
        for (int i = 0; i < 16; i++) lin[i] = Q[(i / 4 / 2) * 2 + (i % 4) / 2] ^ (f & 8 ? 0x00ffffff : 0);
        nvn_swizzle(&tex.d, 0, 0, lin, 16, tex.mem);
        put(OP_DRAW, &dw, sizeof dw);
        run();
        int crop[4] = {0, 0, 64, 64};
        mtl_present(&rt, 1.0 / 60, crop);
        window_pump();
    }
    CHECK(mtl_has_window(), "no window");
    printf("presented 60 frames to the SDL3 window swapchain\n");
#endif
    mtl_finish();
    extern _Atomic int g_vk_validation_errors;
    CHECK(atomic_load(&g_vk_validation_errors) == 0, "%d validation errors", atomic_load(&g_vk_validation_errors));
    printf("draws %llu passes %llu uploads %llu upload bytes %llu readback bytes %llu\n", (unsigned long long)g_stat_draws, (unsigned long long)g_stat_passes,
           (unsigned long long)g_stat_uploads, (unsigned long long)g_stat_upload_bytes, (unsigned long long)g_stat_readback_bytes);
    printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    fflush(stdout);
    _Exit(g_fail ? 1 : 0);   // the completion thread is still parked in its wait
}
