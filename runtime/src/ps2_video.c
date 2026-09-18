#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_statecap.h"
#include "ps2_capture.h"
#include "ps2_vk.h"
#include "ps2_settings.h"
#include "ps2_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

u64 ps2_kernel_vblank_count(void);
void ps2_pad_publish(int port, const ps2_pad_state *st);

#define VK_MAX_VERTS    (1u << 20)
#define VK_MAX_DRAWS    (1u << 16)
#define VK_MAX_TEXTURES 8192
#define VK_TEX_BYTES    (512u << 20)

typedef struct {
    u32 seq;
    u16 topology;
    u16 tex;
    u32 first, count;
    u32 frag_flags;
    u32 blend;
    u32 fix;
    u32 depth;
    float tex_w, tex_h;
    u32 fogcol;
    s32 scissor[4];
    u32 fb_w, fb_h;
    u16 rt;
    u16 tex_rt;
    u16 tex_depth;
    u16 tex_self;
    u16 date;
    u32 wrap;
    u16 minu, maxu, minv, maxv;
    u16 zrt;
    u16 emitter;
    u16 native;
    u16 npoint;
    float nbox[4];
    float nsun_z;
    u32 mvbase, mblock;
    u16 mcull;
} vk_draw;

typedef struct {
    u64 key;
    u32 hash;
    u32 w, h;
    u8 *rgba;
    u8 *rgba_alt;
    u32 vram_base, vram_size;
    u32 generation;
    int uploaded_gen;
    u32 rec_seq;
    u32 levels;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
} vk_texture;

static ps2_vk_vertex *rec_verts;
static vk_draw       *rec_draws;
static u32 rec_nverts, rec_ndraws;
static int rec_overflow;

static vk_texture textures[VK_MAX_TEXTURES];
static u32 ntextures;

#define TEX_HASH 4096u
static s32 tex_head[TEX_HASH], tex_tail[TEX_HASH], tex_next[VK_MAX_TEXTURES];
static int tex_index_ready;

static u32 tex_bucket(u64 key) { return (u32)((key * 0x9E3779B97F4A7C15ull) >> 52); }

static s32 tex_first(u64 key) {
    if (!tex_index_ready) return -1;
    return tex_head[tex_bucket(key)];
}

static void tex_index_add(u32 i) {
    u32 b;
    if (!tex_index_ready) {
        for (u32 k = 0; k < TEX_HASH; k++) tex_head[k] = tex_tail[k] = -1;
        tex_index_ready = 1;
    }
    b = tex_bucket(textures[i].key);
    tex_next[i] = -1;
    if (tex_tail[b] >= 0) tex_next[tex_tail[b]] = (s32)i;
    else tex_head[b] = (s32)i;
    tex_tail[b] = (s32)i;
}
static size_t tex_bytes;

static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  list_cv = PTHREAD_COND_INITIALIZER;
static int frame_requested, frame_done = 1, renderer_running;
int ps2_vk_lockstep;
static volatile int window_closed;
static volatile int swap_dirty;
static u64 stat_dropped;
static u32 stall_drops;
static SDL_ThreadID render_tid;
static int in_modal_redraw;
static volatile int window_minimised;
#define VK_WAIT_NS (100ull * 1000ull * 1000ull)
static int want_fullscreen;
static u32 present_x, present_y, present_w = 640, present_h = 448;
static u32 present_rt;
static int present_blank;
static u32 debug_solid;
static u64 trace_frame;

static u64 stat_frames, stat_draws, stat_verts, stat_texuploads;
float ps2_vk_display_aspect = 4.0f / 3.0f;
static u64 stat_selfreads;
static u64 vk_mono_ns(void);
static u64 speed_pipe_ns, speed_pipe_n;
static int renderer_speed_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PS2_SPEED_LOG");
        enabled = e && atoi(e) != 0;
    }
    return enabled;
}
enum { VS_FENCE, VS_ACQUIRE, VS_RECORD, VS_SUBMIT, VS_PRESENT, VS_TOTAL, VS_N };
static struct {
    u64 start, n, ns[VS_N], peak[VS_N], slow, failed;
    u64 draws, uploads, selfreads, pipe_ns, pipes;
} render_speed;
static void renderer_speed_sample(const u64 *ns, int submitted) {
    u64 now = vk_mono_ns();
    if (!render_speed.start) render_speed.start = now;
    render_speed.n++;
    render_speed.failed += !submitted;
    render_speed.slow += ns[VS_TOTAL] > 16683333ull;
    for (int i = 0; i < VS_N; i++) {
        render_speed.ns[i] += ns[i];
        if (ns[i] > render_speed.peak[i]) render_speed.peak[i] = ns[i];
    }
    if (now - render_speed.start < 5000000000ull) return;
    double n = (double)render_speed.n;
    ps2_log("vk-speed: dt=%.2fs attempts=%llu skipped=%llu host_over_16.68ms=%llu "
            "host_ms avg=%.3f max=%.3f draws/attempt=%.1f uploads=%llu selfread_draws=%llu",
        (now-render_speed.start)/1e9, (unsigned long long)render_speed.n,
        (unsigned long long)render_speed.failed, (unsigned long long)render_speed.slow,
        render_speed.ns[VS_TOTAL]/1e6/n, render_speed.peak[VS_TOTAL]/1e6,
        (stat_draws-render_speed.draws)/n,
        (unsigned long long)(stat_texuploads-render_speed.uploads),
        (unsigned long long)(stat_selfreads-render_speed.selfreads));
    ps2_log("vk-speed: host_ms avg/max fence=%.3f/%.3f acquire=%.3f/%.3f "
            "record=%.3f/%.3f submit=%.3f/%.3f present=%.3f/%.3f "
            "new_pipelines=%llu compile_total_ms=%.3f (included in record; not GPU time)",
        render_speed.ns[VS_FENCE]/1e6/n, render_speed.peak[VS_FENCE]/1e6,
        render_speed.ns[VS_ACQUIRE]/1e6/n, render_speed.peak[VS_ACQUIRE]/1e6,
        render_speed.ns[VS_RECORD]/1e6/n, render_speed.peak[VS_RECORD]/1e6,
        render_speed.ns[VS_SUBMIT]/1e6/n, render_speed.peak[VS_SUBMIT]/1e6,
        render_speed.ns[VS_PRESENT]/1e6/n, render_speed.peak[VS_PRESENT]/1e6,
        (unsigned long long)(speed_pipe_n-render_speed.pipes),
        (speed_pipe_ns-render_speed.pipe_ns)/1e6);
    memset(&render_speed, 0, sizeof(render_speed));
    render_speed.start = now;
    render_speed.draws = stat_draws; render_speed.uploads = stat_texuploads;
    render_speed.selfreads = stat_selfreads;
    render_speed.pipe_ns = speed_pipe_ns; render_speed.pipes = speed_pipe_n;
}

static int env_show_rt = -1, env_present_mode = -1;
static int env_flag(const char *name) {
    const char *v = getenv(name);
    return v ? (int)strtol(v, NULL, 0) : -1;
}

static u32 tex_rec_seq = 2;

static ps2_vk_vertex *rep_verts;
static vk_draw       *rep_draws;

#define LIST_SLOTS 3
static ps2_vk_vertex *slot_verts[LIST_SLOTS];
static vk_draw       *slot_draws[LIST_SLOTS];
static int            rec_slot;
static int            pub_slot = LIST_SLOTS - 1;

#define MESH_MAX_VERTS  (1u << 17)
#define MESH_MAX_INDICES (1u << 19)
#define MESH_MAX_BLOCKS (1u << 14)
#define MESH_REC_BYTES 64u
typedef struct {
    u8 *recs;
    u32 *idx;
    ps2_vk_mesh_block *blocks;
    u32 nrecs, nidx, nblocks;
} mesh_arena;
static mesh_arena slot_mesh[LIST_SLOTS];
static mesh_arena *rec_mesh, *pub_mesh, *rep_mesh;
static int mesh_ok;
static u64 stat_mesh_draws, stat_mesh_overflow, stat_mesh_mat;
static ps2_vk_vertex *pub_verts;
static vk_draw       *pub_draws;
static u32            pub_nverts, pub_ndraws;
static u32 rep_nverts, rep_ndraws;

#define FRAMES_IN_FLIGHT 3
#define RT_MAX 2048u
#define OV_MAX_DRAWS 16384u
#define OV_MAX_VERTS (1u << 18)

typedef struct {
    VkCommandBuffer cmd;
    VkSemaphore acquired, rendered;
    VkFence fence;
    VkQueryPool timing_queries;
    int timing_pending;
    VkBuffer vbuf;
    VkDeviceMemory vmem;
    void *vptr;
    VkBuffer sbuf;
    VkDeviceMemory smem;
    void *sptr;
    size_t soff;
    VkBuffer ovbuf;
    VkDeviceMemory ovmem;
    void *ovptr;
    VkBuffer mvbuf, mibuf, mcbuf;
    VkDeviceMemory mvmem, mimem, mcmem;
    void *mvptr, *miptr, *mcptr;
    VkDescriptorSet mset;
} vk_frame;

static SDL_Window *window;
static char window_title[128];
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static u32 qfamily;
static u32 gpu_timestamp_bits;
static double gpu_timestamp_period;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkFormat swap_format;
static VkExtent2D swap_extent;
static VkImage swap_images[8];
static VkImageView swap_views[8];
static u32 swap_count;
static VkCommandPool cmdpool;
static vk_frame frames[FRAMES_IN_FLIGHT];
static u32 frame_index;

#define RT_SLOTS 32
typedef struct {
    VkImage color, depth;
    VkDeviceMemory cmem, dmem;
    VkImageView cview, dview;
    VkDescriptorSet set;
    VkDescriptorSet dset;
    VkImage snap;
    VkDeviceMemory smem;
    VkImageView sview;
    VkDescriptorSet sset;
    VkImageLayout slayout;
    int snap_valid;
    int snap_ever;
    s32 sdx0, sdy0, sdx1, sdy1;
    int created;
    int drawn;
    int dclear;
    VkImage fimg;
    VkDeviceMemory fmem;
    VkImageView fview;
    VkDescriptorSet fset;
    VkImageLayout flayout;
    VkImageLayout layout;
    VkImageLayout dlayout;
} vk_target;
static vk_target rts[RT_SLOTS];
static u32 rt_w = 640, rt_h = 448;
#define RT_SCALE_MAX 8u
static u32 rt_scale = 1;
static u32 auto_scale_target = 1;
static u32 scale_failed;
static u32 max_image_dim = 4096;
static float max_line_width = 1.0f, max_point_size = 1.0f;
static int depth_blit_ok;
static u32 choose_scale(u32 need_w, u32 need_h);

static VkDescriptorPool despool;
static VkDescriptorSetLayout deslayout;
static VkPipelineLayout gs_layout, present_layout;
static VkShaderModule gs_vs, gs_fs, present_vs, present_fs;
static VkShaderModule mesh_vs, mesh_fs;
static VkDescriptorSetLayout mesh_slayout;
static VkPipeline present_pipe;
static VkShaderModule gs_zfs, colclip_fs;
static VkPipeline colclip_pipe;
static int zcopy_ok, colclip_ok;
static VkShaderModule rn_vis_fs, rn_sun_vs, rn_sun_fs;
static VkPipelineLayout rn_vis_layout, rn_sun_layout;
static VkPipeline rn_vis_pipe, rn_sun_pipe;
static VkImage rn_vis_img;
static VkDeviceMemory rn_vis_mem;
static VkImageView rn_vis_view;
static VkDescriptorSet rn_vis_set;
static VkImageLayout rn_vis_layout_state = VK_IMAGE_LAYOUT_UNDEFINED;
static int rn_native_ok;
static u64 stat_native_draws, stat_native_vis, stat_native_vis_kept, stat_native_vis_none;
static vk_target *cur_zt;
static VkSampler sampler;
static VkSampler sampler_point;
static VkSampler sampler_mip;
static int sampler_aniso;
static float max_aniso;
static VkDescriptorSet white_set;
static VkImage white_img;
static VkDeviceMemory white_mem;
static VkImageView white_view;

extern u64 ps2_gs_prim_seq;
extern u64 ps2_gs_vtx_draw, ps2_gs_vtx_adc;
static u32 draw_limit;
static int draw_limit_read;

static u32 want_draw_limit(void) {
    if (!draw_limit_read) {
        const char *e = getenv("PS2_DRAW_LIMIT");
        draw_limit_read = 1;
        draw_limit = e ? (u32)strtoul(e, NULL, 0) : 0u;
        if (draw_limit) ps2_log("vk: PS2_DRAW_LIMIT=%u -- replaying only the "
                                "primitives before that index", draw_limit);
    }
    return draw_limit;
}

static VkBuffer depth_buf;
static VkDeviceMemory depth_mem;
static void *depth_ptr;
static VkDeviceSize depth_capacity;
static u32 depth_w, depth_h;
static int depth_dump_on = -1;

static int want_depth_dump(void) {
    if (depth_dump_on < 0) {
        const char *e = getenv("PS2_DUMP_DEPTH");
        depth_dump_on = (e && *e && *e != '0') ? 1 : 0;
    }
    return depth_dump_on;
}

static VkBuffer read_buf;
static VkDeviceSize read_capacity;
static VkDeviceMemory read_mem;
static void *read_ptr;
static u32 read_w, read_h, read_slot;
static VkBuffer swap_buf;
static VkDeviceMemory swap_mem;
static void *swap_ptr;
static u32 swap_w, swap_h;

static VkPhysicalDeviceMemoryProperties memprops;
static int vk_ready;
static int rendering_open;

typedef struct { u64 key; VkPipeline pipe; } vk_pipe_entry;
#define PIPE_MAX  2048u
#define PIPE_HASH 4096u
static vk_pipe_entry pipes[PIPE_MAX];
static u32 npipes;
static u16 pipe_slot[PIPE_HASH];

static u32 pipe_bucket(u64 key) {
    return (u32)((key * 0x9E3779B97F4A7C15ull) >> 52) & (PIPE_HASH - 1u);
}

static VkPipelineCache pipe_cache;
static u64 *pipe_warm;
static u32 pipe_warm_n, pipe_warm_i;
static u32 pipe_saved_n;
static u64 pipe_saved_ns;
static u64 stat_pipe_warm;

typedef struct {
    float inv_size[2];
    float tex_size[2];
    u32   flags[4];
    float fogcol[4];
    u32   clamp_uv[4];
    float img_size[2];
    float scale;
    float point_size;
    float tex_scale;
    float pad;
    u32   mesh_block;
    u32   mesh_pad;
} gs_push;

typedef struct { float map[4]; float bound[4]; float opts[4]; float grade[4]; } present_push;

#define VKCHK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    ps2_log("vk: %s failed (%d) at %s:%d", #x, (int)_r, __FILE__, __LINE__); \
    return -1; } } while (0)

static u32 find_mem(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0xFFFFFFFFu;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHK(vkCreateBuffer(dev, &bi, NULL, buf));
    vkGetBufferMemoryRequirements(dev, *buf, &req);
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(req.memoryTypeBits, props);
    VKCHK(vkAllocateMemory(dev, &ai, NULL, mem));
    VKCHK(vkBindBufferMemory(dev, *buf, *mem, 0));
    if (map) VKCHK(vkMapMemory(dev, *mem, 0, size, 0, map));
    return 0;
}

static int make_image(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img,
                      VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vi;
    VkImage li = VK_NULL_HANDLE, ti;
    VkDeviceMemory lm = VK_NULL_HANDLE, tm;
    VkImageView tv;
    VkResult r;
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    *view = VK_NULL_HANDLE;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((r = vkCreateImage(dev, &ii, NULL, &ti)) != VK_SUCCESS) goto fail;
    li = ti;
    vkGetImageMemoryRequirements(dev, li, &req);
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if ((r = vkAllocateMemory(dev, &ai, NULL, &tm)) != VK_SUCCESS) goto fail;
    lm = tm;
    if ((r = vkBindImageMemory(dev, li, lm, 0)) != VK_SUCCESS) goto fail;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = li;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if ((r = vkCreateImageView(dev, &vi, NULL, &tv)) != VK_SUCCESS) goto fail;
    *img = li;
    *mem = lm;
    *view = tv;
    return 0;
fail:
    ps2_log("vk: a %ux%u image could not be created (%d)", w, h, (int)r);
    if (li) vkDestroyImage(dev, li, NULL);
    if (lm) vkFreeMemory(dev, lm, NULL);
    return -1;
}

static void barrier_fill(VkImageMemoryBarrier2 *b, VkImage img,
                         VkImageAspectFlags aspect,
                         VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    memset(b, 0, sizeof(*b));
    b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b->srcStageMask = srcStage;
    b->srcAccessMask = srcAccess;
    b->dstStageMask = dstStage;
    b->dstAccessMask = dstAccess;
    b->oldLayout = from;
    b->newLayout = to;
    b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b->image = img;
    b->subresourceRange.aspectMask = aspect;
    b->subresourceRange.levelCount = 1;
    b->subresourceRange.layerCount = 1;
}

static void barrier_flush(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *b,
                          u32 n) {
    VkDependencyInfo di;
    if (!n) return;
    memset(&di, 0, sizeof(di));
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = n;
    di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &di);
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, img, aspect, from, to, srcStage, srcAccess,
                 dstStage, dstAccess);
    barrier_flush(cmd, &b, 1);
}

static const char *shader_dir(char *buf, size_t cap) {
    static const char *cand[] = { "shaders", "build/gcc/shaders",
                                  "build/shaders", "runtime/shaders" };
    char probe[1024];
    const char *bp;
    unsigned i;
    FILE *f;
    for (i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        snprintf(probe, sizeof probe, "%s/gs.vert.spv", cand[i]);
        f = fopen(probe, "rb");
        if (f) { fclose(f); snprintf(buf, cap, "%s", cand[i]); return buf; }
    }
    bp = SDL_GetBasePath();
    if (bp) {
        snprintf(probe, sizeof probe, "%sshaders/gs.vert.spv", bp);
        f = fopen(probe, "rb");
        if (f) { fclose(f); snprintf(buf, cap, "%sshaders", bp); return buf; }
    }
    snprintf(buf, cap, "shaders");
    return buf;
}

static VkShaderModule load_spv(const char *path) {
    FILE *fp = fopen(path, "rb");
    long n;
    void *buf;
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;
    if (!fp) { ps2_log("vk: cannot open shader '%s'", path); return VK_NULL_HANDLE; }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp); free(buf); return VK_NULL_HANDLE;
    }
    fclose(fp);
    {
        u32 h = 2166136261u;
        long k;
        const char *base = path, *p;
        for (k = 0; k < n; k++) h = (h ^ ((const u8 *)buf)[k]) * 16777619u;
        for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
        ps2_log("vk: shader %s  %ld bytes  fnv=%08X", base, n, h);
    }
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)n;
    ci.pCode = (const uint32_t *)buf;
    if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
        m = VK_NULL_HANDLE;
    free(buf);
    return m;
}

static VkBlendFactor factor_of_c(u32 c, int inverse) {
    switch (c) {
    case 0: return inverse ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
                           : VK_BLEND_FACTOR_SRC1_ALPHA;
    case 1: return inverse ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
                           : VK_BLEND_FACTOR_DST_ALPHA;
    default: return inverse ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
                            : VK_BLEND_FACTOR_CONSTANT_ALPHA;
    }
}

static void translate_blend(u32 a, u32 b, u32 c, u32 d,
                            VkBlendFactor *src, VkBlendFactor *dst,
                            VkBlendOp *op) {
    VkBlendFactor f = factor_of_c(c, 0);
    VkBlendFactor nf = factor_of_c(c, 1);
    *op = VK_BLEND_OP_ADD;

    if (a == b) {
        *src = (d == 0) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        *dst = (d == 1) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        return;
    }

    switch (a * 3u + b) {
    case 0 * 3 + 1:
        if (d == 1) {
            *src = f; *dst = nf; return;
        }
        if (d == 2) {
            *src = f; *dst = f; *op = VK_BLEND_OP_SUBTRACT; return;
        }
        break;
    case 0 * 3 + 2:
        if (d == 1) { *src = f; *dst = VK_BLEND_FACTOR_ONE;  return; }
        if (d == 2) { *src = f; *dst = VK_BLEND_FACTOR_ZERO; return; }
        break;
    case 1 * 3 + 0:
        if (d == 0) {
            *src = nf; *dst = f; return;
        }
        if (d == 2) {
            *src = f; *dst = f; *op = VK_BLEND_OP_REVERSE_SUBTRACT; return;
        }
        break;
    case 1 * 3 + 2:
        if (d == 0) { *src = VK_BLEND_FACTOR_ONE;  *dst = f; return; }
        if (d == 2) { *src = VK_BLEND_FACTOR_ZERO; *dst = f; return; }
        break;
    case 2 * 3 + 0:
        if (d == 0) {
            *src = nf; *dst = VK_BLEND_FACTOR_ZERO; return;
        }
        if (d == 1) {
            *src = f; *dst = VK_BLEND_FACTOR_ONE;
            *op = VK_BLEND_OP_REVERSE_SUBTRACT; return;
        }
        *src = f; *dst = VK_BLEND_FACTOR_ZERO;
        *op = VK_BLEND_OP_REVERSE_SUBTRACT;
        return;
    case 2 * 3 + 1:
        if (d == 0) {
            *src = VK_BLEND_FACTOR_ONE; *dst = f;
            *op = VK_BLEND_OP_SUBTRACT; return;
        }
        if (d == 1) {
            *src = VK_BLEND_FACTOR_ZERO; *dst = nf; return;
        }
        *src = VK_BLEND_FACTOR_ZERO; *dst = f;
        *op = VK_BLEND_OP_REVERSE_SUBTRACT;
        return;
    default:
        break;
    }

    if (a == 0 && b == 1) {
        *src = VK_BLEND_FACTOR_ONE; *dst = f;
        *op = VK_BLEND_OP_SUBTRACT;
        return;
    }
    if (a == 0 && b == 2) {
        *src = VK_BLEND_FACTOR_ONE; *dst = VK_BLEND_FACTOR_ZERO;
        return;
    }
    if (a == 1 && b == 0) {
        *src = f; *dst = VK_BLEND_FACTOR_ONE;
        *op = VK_BLEND_OP_REVERSE_SUBTRACT;
        return;
    }
    *src = VK_BLEND_FACTOR_ZERO; *dst = VK_BLEND_FACTOR_ONE;
}

static int vk_no_ztest = -1, vk_no_fog = -1;

static int vk_env_flag(const char *name, int *cache) {
    if (*cache < 0) {
        const char *e = getenv(name);
        *cache = (e && *e && *e != '0') ? 1 : 0;
        if (*cache) ps2_log("vk: %s is set -- this is a diagnostic override", name);
    }
    return *cache;
}

static VkCompareOp compare_of_ztst(u32 z) {
    if (vk_env_flag("PS2_NO_ZTEST", &vk_no_ztest)) return VK_COMPARE_OP_ALWAYS;
    switch (z) {
    case 0:  return VK_COMPARE_OP_NEVER;
    case 1:  return VK_COMPARE_OP_ALWAYS;
    case 2:  return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_GREATER;
    }
}

static int trace_mask_pipe_env = -1;

static VkPipeline get_pipeline_ex(u32 topology, u32 blend, u32 depth, u32 mesh, u32 cull);
static VkPipeline get_pipeline(u32 topology, u32 blend, u32 depth) {
    return get_pipeline_ex(topology, blend, depth, 0, 0);
}
static VkPipeline get_pipeline_ex(u32 topology, u32 blend, u32 depth, u32 mesh, u32 cull) {
    u64 key = ((u64)topology << 40) | ((u64)blend << 16) | depth
            | ((u64)(mesh & 3u) << 48) | ((u64)(cull & 1u) << 50);
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription bind;
    VkVertexInputAttributeDescription attrs[5];
    VkPipelineVertexInputStateCreateInfo vin;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[4];
    VkPipelineDynamicStateCreateInfo dsi;
    VkPipelineRenderingCreateInfo rci;
    VkGraphicsPipelineCreateInfo gp;
    VkFormat colfmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipeline pipe = VK_NULL_HANDLE;

    u32 h = pipe_bucket(key);
    for (;; h = (h + 1u) & (PIPE_HASH - 1u)) {
        u16 s = pipe_slot[h];
        if (!s) break;
        if (pipes[s - 1u].key == key) return pipes[s - 1u].pipe;
    }
    if (npipes >= PIPE_MAX) return VK_NULL_HANDLE;

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = gs_vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = (depth >> 10) & 1u ? gs_zfs : gs_fs;
    stages[1].pName = "main";

    bind.binding = 0;
    bind.stride = sizeof(ps2_vk_vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = 12;
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[2].offset = 28;
    attrs[3].location = 3; attrs[3].binding = 0;
    attrs[3].format = VK_FORMAT_R32_SFLOAT; attrs[3].offset = 40;
    attrs[4].location = 4; attrs[4].binding = 0;
    attrs[4].format = VK_FORMAT_R32_UINT; attrs[4].offset = 44;

    memset(&vin, 0, sizeof(vin));
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 5;
    vin.pVertexAttributeDescriptions = attrs;
    if (mesh) {
        if (!mesh_vs || ((mesh & 2u) && !mesh_fs)) return VK_NULL_HANDLE;
        stages[0].module = mesh_vs;
        if (mesh & 2u) stages[1].module = mesh_fs;
        bind.stride = MESH_REC_BYTES;
        for (u32 a = 0; a < 4; a++) {
            attrs[a].location = a;
            attrs[a].binding = 0;
            attrs[a].offset = a * 16u;
        }
        attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[1].format = VK_FORMAT_R32G32B32A32_SINT;
        attrs[2].format = VK_FORMAT_R32G32B32A32_SINT;
        attrs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        vin.vertexAttributeDescriptionCount = 4;
    }

    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = topology == PS2_VK_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : topology == PS2_VK_LINES  ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = (depth >> 2) & 1 ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = compare_of_ztst(depth & 3);
    if ((depth >> 10) & 1u) {
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    ds.maxDepthBounds = 1.0f;

    memset(&cba, 0, sizeof(cba));
    if ((depth >> 3) & 1)
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT;
    if ((depth >> 4) & 1)
        cba.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    cba.colorWriteMask &= ~((depth >> 5) & 15u);
    if (blend & 0x100u) {
        VkBlendFactor s, d;
        VkBlendOp o;
        translate_blend(blend & 3u, (blend >> 2) & 3u,
                        (blend >> 4) & 3u, (blend >> 6) & 3u, &s, &d, &o);
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = s;
        cba.dstColorBlendFactor = d;
        cba.colorBlendOp = o;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    if (vk_env_flag("PS2_TRACE_MASK", &trace_mask_pipe_env))
        ps2_log("pipe: topo=%u blend=%03X depth=%02X -> mask=%X blendEnable=%d "
                "(rgb=%d a=%d z=%d ztst=%u)",
                topology, blend, depth, cba.colorWriteMask,
                (int)cba.blendEnable, (depth >> 3) & 1, (depth >> 4) & 1,
                (depth >> 2) & 1, depth & 3);

    dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
    dyn[2] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    dyn[3] = VK_DYNAMIC_STATE_LINE_WIDTH;
    memset(&dsi, 0, sizeof(dsi));
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 4;
    dsi.pDynamicStates = dyn;

    if ((depth >> 9) & 1u) colfmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    memset(&rci, 0, sizeof(rci));
    rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &colfmt;
    rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

    memset(&gp, 0, sizeof(gp));
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dsi;
    gp.layout = gs_layout;

    u64 compile_start = renderer_speed_enabled() ? vk_mono_ns() : 0;
    VkResult compile_result = vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &pipe);
    if (compile_start) {
        speed_pipe_ns += vk_mono_ns() - compile_start;
        speed_pipe_n++;
    }
    if (compile_result != VK_SUCCESS)
        pipe = VK_NULL_HANDLE;
    pipes[npipes].key = key;
    pipes[npipes].pipe = pipe;
    pipe_slot[h] = (u16)(npipes + 1u);
    npipes++;
    return pipe;
}

static const char *pipe_cache_prefix(char *buf, size_t cap) {
    const char *e = getenv("PS2_PIPELINE_CACHE");
    const char *base;
    if (e && *e == '0' && !e[1]) return NULL;
    if (e && *e) { snprintf(buf, cap, "%s", e); return buf; }
    base = SDL_GetBasePath();
    snprintf(buf, cap, "%sac5_pipelines", base ? base : "");
    return buf;
}

static void *pipe_read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    long len;
    void *buf = NULL;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && len < (64l << 20)
        && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)len)) != NULL) {
        if (fread(buf, 1, (size_t)len, f) == (size_t)len) *n = (size_t)len;
        else { free(buf); buf = NULL; }
    }
    fclose(f);
    return buf;
}

static void pipe_write_file(const char *path, const void *a, size_t na,
                            const void *b, size_t nb) {
    char tmp[1100];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    if ((na && fwrite(a, 1, na, f) != na) || (nb && fwrite(b, 1, nb, f) != nb)) {
        fclose(f);
        remove(tmp);
        return;
    }
    fclose(f);
    remove(path);
    rename(tmp, path);
}

#define PIPE_KEYS_MAGIC "AC5PIPE1"

static void pipe_cache_open(void) {
    VkPipelineCacheCreateInfo ci;
    VkPhysicalDeviceProperties pr;
    char prefix[1024], path[1100];
    void *data = NULL;
    size_t n = 0;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (!pipe_cache_prefix(prefix, sizeof prefix)) {
        ps2_log("vk: pipeline cache off (PS2_PIPELINE_CACHE=0)");
        return;
    }
    vkGetPhysicalDeviceProperties(phys, &pr);
    snprintf(path, sizeof path, "%s.cache", prefix);
    data = pipe_read_file(path, &n);
    if (data && n >= 16u + VK_UUID_SIZE) {
        const u8 *p = (const u8 *)data;
        u32 hlen, hver, vendor, device;
        memcpy(&hlen, p, 4); memcpy(&hver, p + 4, 4);
        memcpy(&vendor, p + 8, 4); memcpy(&device, p + 12, 4);
        if (hlen >= 16u + VK_UUID_SIZE && hlen <= n
            && hver == (u32)VK_PIPELINE_CACHE_HEADER_VERSION_ONE
            && vendor == pr.vendorID && device == pr.deviceID
            && !memcmp(p + 16, pr.pipelineCacheUUID, VK_UUID_SIZE)) {
            ci.initialDataSize = n;
            ci.pInitialData = data;
        }
    }
    if (vkCreatePipelineCache(dev, &ci, NULL, &pipe_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(dev, &ci, NULL, &pipe_cache) != VK_SUCCESS)
            pipe_cache = VK_NULL_HANDLE;
    }
    ps2_log("vk: pipeline cache %s (%zu bytes from %s)",
            ci.initialDataSize ? "loaded" : "started empty", ci.initialDataSize,
            ci.initialDataSize ? path : "nothing");
    free(data);

    snprintf(path, sizeof path, "%s.keys", prefix);
    data = pipe_read_file(path, &n);
    if (data && n >= 16u && !memcmp(data, PIPE_KEYS_MAGIC, 8)) {
        u32 count;
        memcpy(&count, (const u8 *)data + 8, 4);
        if (count <= PIPE_MAX && 16u + (size_t)count * 8u <= n
            && (pipe_warm = (u64 *)malloc((size_t)(count ? count : 1u) * 8u)) != NULL) {
            memcpy(pipe_warm, (const u8 *)data + 16, (size_t)count * 8u);
            pipe_warm_n = count;
            ps2_log("vk: %u pipeline states from earlier sessions to build ahead of use",
                    count);
        }
    }
    free(data);
}

static void pipe_warm_step(u64 budget_ns) {
    u64 t0 = vk_mono_ns();
    while (pipe_warm_i < pipe_warm_n) {
        u64 k = pipe_warm[pipe_warm_i++];
        (void)get_pipeline_ex((u32)((k >> 40) & 0xFFu), (u32)((k >> 16) & 0xFFFFFFu),
                              (u32)(k & 0xFFFFu), (u32)((k >> 48) & 3u),
                              (u32)((k >> 50) & 1u));
        stat_pipe_warm++;
        if (vk_mono_ns() - t0 >= budget_ns) break;
    }
    if (pipe_warm_i >= pipe_warm_n && pipe_warm) {
        ps2_log("vk: %llu remembered pipeline states built in advance",
                (unsigned long long)stat_pipe_warm);
        free(pipe_warm);
        pipe_warm = NULL;
        pipe_warm_n = pipe_warm_i = 0;
    }
}

static void pipe_cache_save(int with_data) {
    char prefix[1024], path[1100];
    size_t n = 0;
    void *data;
    u8 hdr[16];
    u64 *keys;
    u32 count = 0;
    if (!pipe_cache || !pipe_cache_prefix(prefix, sizeof prefix)) return;
    if (with_data && vkGetPipelineCacheData(dev, pipe_cache, &n, NULL) == VK_SUCCESS && n
        && (data = malloc(n)) != NULL) {
        if (vkGetPipelineCacheData(dev, pipe_cache, &n, data) == VK_SUCCESS) {
            snprintf(path, sizeof path, "%s.cache", prefix);
            pipe_write_file(path, data, n, NULL, 0);
        }
        free(data);
    }
    keys = (u64 *)malloc((size_t)(npipes ? npipes : 1u) * 8u);
    if (!keys) return;
    for (u32 i = 0; i < npipes; i++)
        if (pipes[i].pipe) keys[count++] = pipes[i].key;
    memcpy(hdr, PIPE_KEYS_MAGIC, 8);
    memcpy(hdr + 8, &count, 4);
    memset(hdr + 12, 0, 4);
    snprintf(path, sizeof path, "%s.keys", prefix);
    pipe_write_file(path, hdr, sizeof hdr, keys, (size_t)count * 8u);
    free(keys);
    pipe_saved_n = npipes;
    pipe_saved_ns = vk_mono_ns();
}

static VkPresentModeKHR pick_present_mode(void) {
    VkPresentModeKHR modes[16];
    u32 n = 16, i;
    VkPresentModeKHR want = VK_PRESENT_MODE_MAILBOX_KHR;
    const char *e = getenv("PS2_PRESENT_MODE");
    switch (ps2_cfg.present_mode) {
    case PS2_PRESENT_FIFO:         want = VK_PRESENT_MODE_FIFO_KHR; break;
    case PS2_PRESENT_IMMEDIATE:    want = VK_PRESENT_MODE_IMMEDIATE_KHR; break;
    case PS2_PRESENT_FIFO_RELAXED: want = VK_PRESENT_MODE_FIFO_RELAXED_KHR; break;
    default:                       break;
    }
    if (e) {
        if (!strcmp(e, "fifo"))           want = VK_PRESENT_MODE_FIFO_KHR;
        else if (!strcmp(e, "mailbox"))   want = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (!strcmp(e, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!strcmp(e, "relaxed"))   want = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    if (want == VK_PRESENT_MODE_FIFO_KHR
        || vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes)
           != VK_SUCCESS)
        n = 0;
    for (i = 0; i < n; i++)
        if (modes[i] == want) {
            if (env_present_mode != (int)want) {
                env_present_mode = (int)want;
                ps2_log("vk: present mode %s",
                        want == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                      : want == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE"
                      : "FIFO_RELAXED");
            }
            return want;
        }
    if (env_present_mode != (int)VK_PRESENT_MODE_FIFO_KHR) {
        env_present_mode = (int)VK_PRESENT_MODE_FIFO_KHR;
        ps2_log("vk: present mode FIFO (vsync)%s",
                want == VK_PRESENT_MODE_FIFO_KHR ? ""
                : " -- the surface does not offer the mode asked for");
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR ci;
    VkSurfaceFormatKHR fmts[64];
    u32 nf = 64;
    int w = 0, h = 0;

    VKCHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    SDL_GetWindowSizeInPixels(window, &w, &h);
    swap_extent = caps.currentExtent;
    if (swap_extent.width == 0xFFFFFFFFu) {
        swap_extent.width = (u32)w;
        swap_extent.height = (u32)h;
    }
    if (swap_extent.width == 0 || swap_extent.height == 0) return 1;

    VKCHK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, fmts));
    swap_format = fmts[0].format;
    for (u32 i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { swap_format = fmts[i].format; break; }

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = caps.minImageCount < FRAMES_IN_FLIGHT + 1u
                     ? FRAMES_IN_FLIGHT + 1u : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pick_present_mode();
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VKCHK(vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain));
    if (ci.oldSwapchain) {
        for (u32 i = 0; i < swap_count; i++)
            vkDestroyImageView(dev, swap_views[i], NULL);
        vkDestroySwapchainKHR(dev, ci.oldSwapchain, NULL);
    }
    swap_count = 8;
    VKCHK(vkGetSwapchainImagesKHR(dev, swapchain, &swap_count, swap_images));
    ps2_ui_swapchain_changed(swap_count);
    for (u32 i = 0; i < swap_count; i++) {
        VkImageViewCreateInfo vi;
        memset(&vi, 0, sizeof(vi));
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VKCHK(vkCreateImageView(dev, &vi, NULL, &swap_views[i]));
    }
    return 0;
}

static VkResult make_linear_sampler(VkSampler *out, int aniso) {
    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 1.0f;
    if (aniso >= 2 && max_aniso >= 2.0f) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = (float)aniso < max_aniso ? (float)aniso : max_aniso;
    }
    sampler_aniso = aniso;
    return vkCreateSampler(dev, &sci, NULL, out);
}

static VkDescriptorSet alloc_set_s(VkImageView view, VkSampler smp) {
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorImageInfo di;
    VkWriteDescriptorSet w;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = despool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &deslayout;
    if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    memset(&di, 0, sizeof(di));
    di.sampler = smp;
    di.imageView = view;
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof(w));
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    return set;
}

static VkDescriptorSet alloc_set(VkImageView view) {
    return alloc_set_s(view, sampler);
}

static void rebind_set_s(VkDescriptorSet set, VkImageView view, VkSampler smp) {
    VkDescriptorImageInfo di;
    VkWriteDescriptorSet w;
    if (!set || !view) return;
    memset(&di, 0, sizeof(di));
    di.sampler = smp;
    di.imageView = view;
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&w, 0, sizeof(w));
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
}

static void rebind_set(VkDescriptorSet set, VkImageView view) {
    rebind_set_s(set, view, sampler);
}

static void apply_sampler_settings(void) {
    VkSampler fresh, old = sampler;
    if (ps2_cfg.anisotropy == sampler_aniso) return;
    if (make_linear_sampler(&fresh, ps2_cfg.anisotropy) != VK_SUCCESS) return;
    vkDeviceWaitIdle(dev);
    sampler = fresh;
    rebind_set(white_set, white_view);
    for (u32 i = 0; i < ntextures && i < VK_MAX_TEXTURES; i++)
        rebind_set(textures[i].set, textures[i].view);
    for (u32 k = 0; k < RT_SLOTS; k++) {
        if (!rts[k].created) continue;
        rebind_set(rts[k].set, rts[k].cview);
        rebind_set(rts[k].sset, rts[k].sview);
    }
    vkDestroySampler(dev, old, NULL);
    ps2_log("vk: anisotropic filtering %dx", sampler_aniso >= 2 ? sampler_aniso : 0);
}

static void destroy_target_images(vk_target *t) {
    if (t->snap) {
        vkDestroyImageView(dev, t->sview, NULL);
        vkDestroyImage(dev, t->snap, NULL);
        vkFreeMemory(dev, t->smem, NULL);
    }
    if (t->cview) vkDestroyImageView(dev, t->cview, NULL);
    if (t->color) vkDestroyImage(dev, t->color, NULL);
    if (t->cmem)  vkFreeMemory(dev, t->cmem, NULL);
    if (t->dview) vkDestroyImageView(dev, t->dview, NULL);
    if (t->depth) vkDestroyImage(dev, t->depth, NULL);
    if (t->dmem)  vkFreeMemory(dev, t->dmem, NULL);
    if (t->fview) vkDestroyImageView(dev, t->fview, NULL);
    if (t->fimg)  vkDestroyImage(dev, t->fimg, NULL);
    if (t->fmem)  vkFreeMemory(dev, t->fmem, NULL);
    t->fimg = VK_NULL_HANDLE;
    t->fmem = VK_NULL_HANDLE;
    t->fview = VK_NULL_HANDLE;
    t->flayout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->snap = t->color = t->depth = VK_NULL_HANDLE;
    t->smem = t->cmem = t->dmem = VK_NULL_HANDLE;
    t->sview = t->cview = t->dview = VK_NULL_HANDLE;
    t->created = 0;
}

static int create_target(vk_target *t) {
    u32 iw = rt_w * rt_scale, ih = rt_h * rt_scale;
    if (t->created) return 0;
    if (make_image(iw, ih, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT,
                   &t->color, &t->cmem, &t->cview) != 0) {
        destroy_target_images(t);
        return -1;
    }
    if (make_image(iw, ih, VK_FORMAT_D32_SFLOAT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT,
                   &t->depth, &t->dmem, &t->dview) != 0) {
        destroy_target_images(t);
        return -1;
    }
    if (t->set) rebind_set(t->set, t->cview);
    else t->set = alloc_set(t->cview);
    if (t->dset) rebind_set_s(t->dset, t->dview, sampler_point);
    else t->dset = alloc_set_s(t->dview, sampler_point);
    t->snap = VK_NULL_HANDLE;
    t->snap_valid = 0;
    t->snap_ever = 0;
    t->sdx0 = (s32)rt_w; t->sdy0 = (s32)rt_h;
    t->sdx1 = 0; t->sdy1 = 0;
    t->slayout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->created = 1;
    t->drawn = 0;
    t->dclear = 0;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->dlayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 0;
}

static void copy_target_pictures(const vk_target *old, u32 ow, u32 oh, u32 os) {
    static const VkImageSubresourceRange crange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    static const VkImageSubresourceRange drange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    u32 i;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdpool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) goto done;
    for (i = 0; i < RT_SLOTS; i++) {
        const vk_target *o = &old[i];
        vk_target *t = &rts[i];
        VkImageBlit b;
        if (!o->created || !t->created || !o->drawn
            || o->layout == VK_IMAGE_LAYOUT_UNDEFINED)
            continue;
        memset(&b, 0, sizeof b);
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[1].x = (s32)(ow * os);
        b.srcOffsets[1].y = (s32)(oh * os);
        b.srcOffsets[1].z = 1;
        b.dstSubresource.layerCount = 1;
        b.dstOffsets[1].x = (s32)(ow * rt_scale);
        b.dstOffsets[1].y = (s32)(oh * rt_scale);
        b.dstOffsets[1].z = 1;
        {
            VkClearColorValue black;
            memset(&black, 0, sizeof black);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &crange);
            barrier(cmd, o->color, VK_IMAGE_ASPECT_COLOR_BIT,
                    o->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdBlitImage(cmd, o->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &b, VK_FILTER_LINEAR);
            barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
            t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            t->drawn = 1;
        }
        if (depth_blit_ok && o->dlayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            VkClearDepthStencilValue far_plane;
            memset(&far_plane, 0, sizeof far_plane);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier(cmd, t->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdClearDepthStencilImage(cmd, t->depth,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &far_plane, 1, &drange);
            barrier(cmd, o->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                    o->dlayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdBlitImage(cmd, o->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &b, VK_FILTER_NEAREST);
            barrier(cmd, t->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            t->dlayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        } else {
            t->dclear = 1;
        }
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) goto done;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS)
        vkQueueWaitIdle(queue);
done:
    vkFreeCommandBuffers(dev, cmdpool, 1, &cmd);
}

static int create_targets(u32 w, u32 h, u32 scale) {
    static vk_target old[RT_SLOTS];
    u32 ow = rt_w, oh = rt_h, os = rt_scale, i, k;
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > RT_MAX) w = RT_MAX;
    if (h > RT_MAX) h = RT_MAX;
    if (w < rt_w) w = rt_w;
    if (h < rt_h) h = rt_h;
    if (scale < 1) scale = 1;
    if (rts[0].created && w == rt_w && h == rt_h && scale == rt_scale) return 0;
    if (!rts[0].created) {
        rt_w = w;
        rt_h = h;
        rt_scale = scale;
        if (create_target(&rts[0]) == 0) return 0;
        rt_scale = os;
        scale_failed = scale;
        return -1;
    }
    vkDeviceWaitIdle(dev);
    memcpy(old, rts, sizeof old);
    for (i = 0; i < RT_SLOTS; i++) {
        memset(&rts[i], 0, sizeof rts[i]);
        rts[i].set = old[i].set;
        rts[i].dset = old[i].dset;
        rts[i].sset = old[i].sset;
        rts[i].fset = old[i].fset;
    }
    rt_w = w;
    rt_h = h;
    rt_scale = scale;
    for (i = 0; i < RT_SLOTS; i++)
        if (old[i].created && create_target(&rts[i]) != 0) break;
    if (i < RT_SLOTS) {
        for (k = 0; k < RT_SLOTS; k++) destroy_target_images(&rts[k]);
        memcpy(rts, old, sizeof rts);
        rt_w = ow;
        rt_h = oh;
        rt_scale = os;
        for (k = 0; k < RT_SLOTS; k++) {
            if (!rts[k].created) continue;
            rebind_set(rts[k].set, rts[k].cview);
            rebind_set_s(rts[k].dset, rts[k].dview, sampler_point);
            if (rts[k].snap) rebind_set(rts[k].sset, rts[k].sview);
            if (rts[k].fimg) rebind_set_s(rts[k].fset, rts[k].fview, sampler_point);
        }
        scale_failed = scale;
        ps2_log("vk: render targets of %ux%u at %ux do not fit; staying at %ux",
                w, h, scale, os);
        return -1;
    }
    copy_target_pictures(old, ow, oh, os);
    for (i = 0; i < RT_SLOTS; i++) destroy_target_images(&old[i]);
    ps2_log("vk: render targets %ux%u at %ux internal resolution (%ux%u images)",
            rt_w, rt_h, rt_scale, rt_w * rt_scale, rt_h * rt_scale);
    return 0;
}

static int vk_init(void) {
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkDeviceCreateInfo dci;
    VkDeviceQueueCreateInfo qci;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDevice devs[16];
    u32 ndev = 16, nq;
    VkQueueFamilyProperties qprops[16];
    const char *const *sdl_exts;
    Uint32 nsdl = 0;
    const char *dev_exts[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    float prio = 1.0f;
    VkDescriptorPoolSize psz;
    VkDescriptorPoolCreateInfo pci;
    VkDescriptorSetLayoutBinding dlb;
    VkDescriptorSetLayoutCreateInfo dlc;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo plc;
    VkSamplerCreateInfo sci;
    VkCommandPoolCreateInfo cpi;
    VkCommandBufferAllocateInfo cbi;
    char spv[1280];
    const char *base = getenv("PS2_SHADER_DIR");
    char basebuf[1024];
    if (!base) base = shader_dir(basebuf, sizeof basebuf);

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ps2recomp";
    app.apiVersion = VK_API_VERSION_1_3;

    sdl_exts = SDL_Vulkan_GetInstanceExtensions(&nsdl);
    if (!sdl_exts) { ps2_log("vk: SDL has no Vulkan extensions"); return -1; }

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = nsdl;
    ici.ppEnabledExtensionNames = sdl_exts;
    VKCHK(vkCreateInstance(&ici, NULL, &inst));

    if (!SDL_Vulkan_CreateSurface(window, inst, NULL, &surface)) {
        ps2_log("vk: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return -1;
    }

    VKCHK(vkEnumeratePhysicalDevices(inst, &ndev, devs));
    phys = VK_NULL_HANDLE;
    for (u32 i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(devs[i], &pr);
        if (pr.apiVersion < VK_API_VERSION_1_3) continue;
        if (phys == VK_NULL_HANDLE
            || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            phys = devs[i];
    }
    if (!phys) { ps2_log("vk: no Vulkan 1.3 device"); return -1; }
    {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(phys, &pr);
        gpu_timestamp_period = pr.limits.timestampPeriod;
        max_aniso = pr.limits.maxSamplerAnisotropy;
        max_image_dim = pr.limits.maxImageDimension2D;
        max_line_width = pr.limits.lineWidthRange[1];
        max_point_size = pr.limits.pointSizeRange[1];
        ps2_log("vk: %s (API %u.%u.%u)", pr.deviceName,
                VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion),
                VK_VERSION_PATCH(pr.apiVersion));
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qprops);
    qfamily = 0xFFFFFFFFu;
    for (u32 i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            qfamily = i;
            break;
        }
    }
    if (qfamily == 0xFFFFFFFFu) { ps2_log("vk: no graphics+present queue"); return -1; }

    gpu_timestamp_bits = qprops[qfamily].timestampValidBits;
    if (renderer_speed_enabled())
        ps2_log("gpu-speed: timestamps %s, valid_bits=%u period_ns=%.6f; queried after existing frame fences",
            gpu_timestamp_bits ? "enabled" : "unsupported", gpu_timestamp_bits, gpu_timestamp_period);
    memset(&f13, 0, sizeof(f13));
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    memset(&f2, 0, sizeof(f2));
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    {
        VkPhysicalDeviceFeatures supported;
        vkGetPhysicalDeviceFeatures(phys, &supported);
        if (!supported.dualSrcBlend) {
            ps2_log("vk: dual-source blending is required for GS framebuffer alpha");
            return -1;
        }
        f2.features.dualSrcBlend = VK_TRUE;
        if (supported.samplerAnisotropy) f2.features.samplerAnisotropy = VK_TRUE;
        else max_aniso = 0.0f;
        if (supported.wideLines) f2.features.wideLines = VK_TRUE;
        else max_line_width = 1.0f;
        if (supported.largePoints) f2.features.largePoints = VK_TRUE;
        else max_point_size = 1.0f;
        if (max_line_width < 1.0f) max_line_width = 1.0f;
        if (max_point_size < 1.0f) max_point_size = 1.0f;
    }
    {
        VkFormatProperties fp;
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT
                                  | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D32_SFLOAT, &fp);
        depth_blit_ok = (fp.optimalTilingFeatures & need) == need;
        ps2_log("vk: internal resolution: images up to %u px, lines up to %.1f px "
                "wide, points up to %.1f px, depth %s on a rebuild",
                max_image_dim, (double)max_line_width, (double)max_point_size,
                depth_blit_ok ? "kept" : "cleared");
    }

    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VKCHK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfamily, 0, &queue);
    pipe_cache_open();

    if (create_swapchain() < 0) return -1;

    memset(&cpi, 0, sizeof(cpi));
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qfamily;
    VKCHK(vkCreateCommandPool(dev, &cpi, NULL, &cmdpool));

    psz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz.descriptorCount = VK_MAX_TEXTURES + RT_SLOTS * 3u + 32u;
    {
        VkDescriptorPoolSize sizes[2];
        sizes[0] = psz;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = FRAMES_IN_FLIGHT;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = VK_MAX_TEXTURES + RT_SLOTS * 3u + 32u + FRAMES_IN_FLIGHT;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        VKCHK(vkCreateDescriptorPool(dev, &pci, NULL, &despool));
    }

    memset(&dlb, 0, sizeof(dlb));
    dlb.binding = 0;
    dlb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dlb.descriptorCount = 1;
    dlb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&dlc, 0, sizeof(dlc));
    dlc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlc.bindingCount = 1;
    dlc.pBindings = &dlb;
    VKCHK(vkCreateDescriptorSetLayout(dev, &dlc, NULL, &deslayout));

    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(gs_push);
    memset(&plc, 0, sizeof(plc));
    plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    {   VkDescriptorSetLayoutBinding mb;
        VkDescriptorSetLayoutCreateInfo mc;
        memset(&mb, 0, sizeof(mb));
        mb.binding = 0;
        mb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mb.descriptorCount = 1;
        mb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        memset(&mc, 0, sizeof(mc));
        mc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        mc.bindingCount = 1;
        mc.pBindings = &mb;
        VKCHK(vkCreateDescriptorSetLayout(dev, &mc, NULL, &mesh_slayout));
    }
    {   VkDescriptorSetLayout sl[3];
        sl[0] = deslayout; sl[1] = deslayout; sl[2] = mesh_slayout;
        plc.setLayoutCount = 3;
        plc.pSetLayouts = sl;
        plc.pushConstantRangeCount = 1;
        plc.pPushConstantRanges = &pcr;
        VKCHK(vkCreatePipelineLayout(dev, &plc, NULL, &gs_layout));
    }
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &deslayout;
    pcr.size = sizeof(present_push);
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VKCHK(vkCreatePipelineLayout(dev, &plc, NULL, &present_layout));

    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 1.0f;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    VKCHK(vkCreateSampler(dev, &sci, NULL, &sampler_point));
    VKCHK(make_linear_sampler(&sampler, ps2_cfg.anisotropy));
    {   VkSamplerCreateInfo ms;
        memset(&ms, 0, sizeof(ms));
        ms.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ms.magFilter = VK_FILTER_LINEAR;
        ms.minFilter = VK_FILTER_LINEAR;
        ms.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ms.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ms.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ms.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ms.maxLod = VK_LOD_CLAMP_NONE;
        if (max_aniso >= 2.0f) {
            float want = ps2_cfg.anisotropy >= 2 ? (float)ps2_cfg.anisotropy : 16.0f;
            ms.anisotropyEnable = VK_TRUE;
            ms.maxAnisotropy = want < max_aniso ? want : max_aniso;
        }
        VKCHK(vkCreateSampler(dev, &ms, NULL, &sampler_mip));
    }

    snprintf(spv, sizeof(spv), "%s/gs.vert.spv", base);
    gs_vs = load_spv(spv);
    snprintf(spv, sizeof(spv), "%s/gs.frag.spv", base);
    gs_fs = load_spv(spv);
    snprintf(spv, sizeof(spv), "%s/present.vert.spv", base);
    present_vs = load_spv(spv);
    snprintf(spv, sizeof(spv), "%s/present.frag.spv", base);
    present_fs = load_spv(spv);
    if (!gs_vs || !gs_fs || !present_vs || !present_fs) {
        ps2_log("vk: shader modules missing (looked in '%s')", base);
        return -1;
    }
    snprintf(spv, sizeof(spv), "%s/mesh.vert.spv", base);
    mesh_vs = load_spv(spv);
    snprintf(spv, sizeof(spv), "%s/mesh.frag.spv", base);
    mesh_fs = load_spv(spv);
    if (!mesh_vs) ps2_log("vk: mesh.vert missing; native meshes are drawn as GS primitives");
    snprintf(spv, sizeof(spv), "%s/gs_zcopy.frag.spv", base);
    gs_zfs = load_spv(spv);
    snprintf(spv, sizeof(spv), "%s/colclip.frag.spv", base);
    colclip_fs = load_spv(spv);
    zcopy_ok = gs_zfs != VK_NULL_HANDLE;
    {   VkFormatProperties fp;
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                                  | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                                  | VK_FORMAT_FEATURE_BLIT_DST_BIT
                                  | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
        colclip_ok = colclip_fs != VK_NULL_HANDLE
                  && (fp.optimalTilingFeatures & need) == need;
        if (!colclip_ok)
            ps2_log("vk: wrapping blends will clamp -- %s",
                    colclip_fs ? "the device cannot blend into RGBA32F"
                               : "colclip.frag.spv is missing");
    }

    memset(&cbi, 0, sizeof(cbi));
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = cmdpool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkSemaphoreCreateInfo si;
        VkFenceCreateInfo fi;
        VKCHK(vkAllocateCommandBuffers(dev, &cbi, &frames[i].cmd));
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VKCHK(vkCreateSemaphore(dev, &si, NULL, &frames[i].acquired));
        VKCHK(vkCreateSemaphore(dev, &si, NULL, &frames[i].rendered));
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHK(vkCreateFence(dev, &fi, NULL, &frames[i].fence));
        if (renderer_speed_enabled() && gpu_timestamp_bits) {
            VkQueryPoolCreateInfo qi = {0};
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 3;
            if (vkCreateQueryPool(dev, &qi, NULL, &frames[i].timing_queries) != VK_SUCCESS)
                ps2_log("gpu-speed: timestamp query allocation failed; host timing remains available");
        }

        if (make_buffer((VkDeviceSize)VK_MAX_VERTS * sizeof(ps2_vk_vertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &frames[i].vbuf, &frames[i].vmem, &frames[i].vptr) != 0)
            return -1;
        if (make_buffer(16u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &frames[i].sbuf, &frames[i].smem, &frames[i].sptr) != 0)
            return -1;
        if (make_buffer((VkDeviceSize)OV_MAX_VERTS * sizeof(ps2_vk_vertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &frames[i].ovbuf, &frames[i].ovmem, &frames[i].ovptr) != 0)
            return -1;
        if (mesh_vs
            && make_buffer((VkDeviceSize)MESH_MAX_VERTS * MESH_REC_BYTES,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &frames[i].mvbuf, &frames[i].mvmem, &frames[i].mvptr) == 0
            && make_buffer((VkDeviceSize)MESH_MAX_INDICES * sizeof(u32),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &frames[i].mibuf, &frames[i].mimem, &frames[i].miptr) == 0
            && make_buffer((VkDeviceSize)MESH_MAX_BLOCKS * sizeof(ps2_vk_mesh_block),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &frames[i].mcbuf, &frames[i].mcmem, &frames[i].mcptr) == 0) {
            VkDescriptorSetAllocateInfo ai;
            memset(&ai, 0, sizeof(ai));
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = despool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &mesh_slayout;
            if (vkAllocateDescriptorSets(dev, &ai, &frames[i].mset) == VK_SUCCESS) {
                VkDescriptorBufferInfo bi;
                VkWriteDescriptorSet w;
                bi.buffer = frames[i].mcbuf;
                bi.offset = 0;
                bi.range = VK_WHOLE_SIZE;
                memset(&w, 0, sizeof(w));
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = frames[i].mset;
                w.dstBinding = 0;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &bi;
                vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
                if (i == FRAMES_IN_FLIGHT - 1) mesh_ok = 1;
            }
        }
    }
    if (mesh_vs && !mesh_ok)
        ps2_log("vk: native mesh buffers could not be made; meshes are drawn as GS primitives");

    if (make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT,
                   &white_img, &white_mem, &white_view) != 0) return -1;
    white_set = alloc_set(white_view);

    {
        u32 s;
        auto_scale_target = (swap_extent.height + 447u) / 448u;
        s = choose_scale(rt_w, rt_h);
        if (create_targets(rt_w, rt_h, s) != 0
            && (s == 1u || create_targets(rt_w, rt_h, 1u) != 0))
            return -1;
    }
    depth_capacity = (VkDeviceSize)4096 * 4096 * 4;
    if (want_depth_dump()
        && make_buffer(depth_capacity,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &depth_buf, &depth_mem, &depth_ptr) != 0) return -1;
    if (make_buffer((VkDeviceSize)4096 * 4096 * 4,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &swap_buf, &swap_mem, &swap_ptr) != 0) return -1;

    {
        VkPipelineShaderStageCreateInfo st[2];
        VkPipelineVertexInputStateCreateInfo vin;
        VkPipelineInputAssemblyStateCreateInfo ia;
        VkPipelineViewportStateCreateInfo vp;
        VkPipelineRasterizationStateCreateInfo rs;
        VkPipelineMultisampleStateCreateInfo ms;
        VkPipelineColorBlendAttachmentState cba;
        VkPipelineColorBlendStateCreateInfo cb;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT,
                                  VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi;
        VkPipelineRenderingCreateInfo rci;
        VkGraphicsPipelineCreateInfo gp;

        memset(st, 0, sizeof(st));
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = present_vs;
        st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = present_fs;
        st[1].pName = "main";
        memset(&vin, 0, sizeof(vin));
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        memset(&ia, 0, sizeof(ia));
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        memset(&vp, 0, sizeof(vp));
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        memset(&rs, 0, sizeof(rs));
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        memset(&ms, 0, sizeof(ms));
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&cba, 0, sizeof(cba));
        cba.colorWriteMask = 0xF;
        memset(&cb, 0, sizeof(cb));
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        memset(&dsi, 0, sizeof(dsi));
        dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsi.dynamicStateCount = 2;
        dsi.pDynamicStates = dyn;
        memset(&rci, 0, sizeof(rci));
        rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rci.colorAttachmentCount = 1;
        rci.pColorAttachmentFormats = &swap_format;
        memset(&gp, 0, sizeof(gp));
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.pNext = &rci;
        gp.stageCount = 2;
        gp.pStages = st;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsi;
        gp.layout = present_layout;
        VKCHK(vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL,
                                        &present_pipe));

        {   VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
            VkPushConstantRange r;
            VkPipelineLayoutCreateInfo li;
            VkDescriptorSetLayout sl[2] = { deslayout, deslayout };
            snprintf(spv, sizeof(spv), "%s/rn_vis.frag.spv", base);
            rn_vis_fs = load_spv(spv);
            snprintf(spv, sizeof(spv), "%s/rn_sun.vert.spv", base);
            rn_sun_vs = load_spv(spv);
            snprintf(spv, sizeof(spv), "%s/rn_sun.frag.spv", base);
            rn_sun_fs = load_spv(spv);
            if (rn_vis_fs && rn_sun_vs && rn_sun_fs) {
                memset(&r, 0, sizeof r);
                r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                r.size = 48;
                memset(&li, 0, sizeof li);
                li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                li.setLayoutCount = 2;
                li.pSetLayouts = sl;
                li.pushConstantRangeCount = 1;
                li.pPushConstantRanges = &r;
                if (vkCreatePipelineLayout(dev, &li, NULL, &rn_vis_layout) == VK_SUCCESS
                    && vkCreatePipelineLayout(dev, &li, NULL, &rn_sun_layout) == VK_SUCCESS
                    && make_image(1, 1, rgba8,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                  | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, &rn_vis_img,
                                  &rn_vis_mem, &rn_vis_view) == 0
                    && (rn_vis_set = alloc_set_s(rn_vis_view, sampler_point))) {
                    st[1].module = rn_vis_fs;
                    rci.pColorAttachmentFormats = &rgba8;
                    rci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
                    gp.pDepthStencilState = NULL;
                    gp.layout = rn_vis_layout;
                    cba.blendEnable = VK_FALSE;
                    cba.colorWriteMask = 0xF;
                    if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL,
                                                  &rn_vis_pipe) == VK_SUCCESS) {
                        VkVertexInputBindingDescription bind;
                        VkVertexInputAttributeDescription at[5];
                        VkPipelineVertexInputStateCreateInfo svin;
                        VkPipelineColorBlendAttachmentState sba = cba;
                        VkPipelineColorBlendStateCreateInfo scb = cb;
                        VkGraphicsPipelineCreateInfo sgp = gp;
                        VkPipelineShaderStageCreateInfo sst[2];
                        bind.binding = 0;
                        bind.stride = sizeof(ps2_vk_vertex);
                        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                        at[0].location = 0; at[0].binding = 0;
                        at[0].format = VK_FORMAT_R32G32B32_SFLOAT; at[0].offset = 0;
                        at[1].location = 1; at[1].binding = 0;
                        at[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; at[1].offset = 12;
                        at[2].location = 2; at[2].binding = 0;
                        at[2].format = VK_FORMAT_R32G32B32_SFLOAT; at[2].offset = 28;
                        at[3].location = 3; at[3].binding = 0;
                        at[3].format = VK_FORMAT_R32_SFLOAT; at[3].offset = 40;
                        at[4].location = 4; at[4].binding = 0;
                        at[4].format = VK_FORMAT_R32_UINT; at[4].offset = 44;
                        memset(&svin, 0, sizeof svin);
                        svin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                        svin.vertexBindingDescriptionCount = 1;
                        svin.pVertexBindingDescriptions = &bind;
                        svin.vertexAttributeDescriptionCount = 5;
                        svin.pVertexAttributeDescriptions = at;
                        sba.blendEnable = VK_TRUE;
                        sba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                        sba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                        sba.colorBlendOp = VK_BLEND_OP_ADD;
                        sba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                        sba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                        sba.alphaBlendOp = VK_BLEND_OP_ADD;
                        sba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                                           | VK_COLOR_COMPONENT_G_BIT
                                           | VK_COLOR_COMPONENT_B_BIT;
                        scb.pAttachments = &sba;
                        sst[0] = st[0]; sst[1] = st[1];
                        sst[0].module = rn_sun_vs;
                        sst[1].module = rn_sun_fs;
                        sgp.pStages = sst;
                        sgp.pVertexInputState = &svin;
                        sgp.pColorBlendState = &scb;
                        sgp.layout = rn_sun_layout;
                        if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &sgp,
                                                      NULL, &rn_sun_pipe) == VK_SUCCESS)
                            rn_native_ok = 1;
                    }
                    cba.blendEnable = VK_FALSE;
                    cba.colorWriteMask = 0xF;
                }
            }
            if (!rn_native_ok)
                ps2_log("vk: native passes unavailable (rn_vis.frag / rn_sun.vert / "
                        "rn_sun.frag missing or refused); claimed draws are dropped "
                        "without a replacement");
            st[1].module = present_fs;
            gp.layout = present_layout;
            gp.pDepthStencilState = NULL;
            rci.pColorAttachmentFormats = &swap_format;
            rci.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        }

        if (colclip_ok) {
            VkPipelineDepthStencilStateCreateInfo ds;
            VkFormat rgba8 = VK_FORMAT_R8G8B8A8_UNORM;
            memset(&ds, 0, sizeof(ds));
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.maxDepthBounds = 1.0f;
            st[1].module = colclip_fs;
            rci.pColorAttachmentFormats = &rgba8;
            rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
            gp.pDepthStencilState = &ds;
            if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL,
                                          &colclip_pipe) != VK_SUCCESS) {
                colclip_ok = 0;
                ps2_log("vk: wrapping blends will clamp -- resolve pipeline failed");
            }
        }
    }
    return 0;
}

#define TEXMEM_BLOCK      (64ull << 20)
#define TEXMEM_MAX_BLOCKS 64u
static struct { VkDeviceMemory mem; VkDeviceSize used; u32 type; } texmem[TEXMEM_MAX_BLOCKS];
static u32 ntexmem;
static u64 stat_texmem_own;

static int texmem_bind(VkImage img, VkDeviceMemory *mem) {
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    u32 type, i;
    vkGetImageMemoryRequirements(dev, img, &req);
    type = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) return -1;
    if (req.size <= TEXMEM_BLOCK / 4u) {
        VkDeviceSize a = req.alignment ? req.alignment : 1u;
        for (i = ntexmem; i-- > 0;) {
            VkDeviceSize off = (texmem[i].used + a - 1u) / a * a;
            if (texmem[i].type != type || off + req.size > TEXMEM_BLOCK) continue;
            if (vkBindImageMemory(dev, img, texmem[i].mem, off) != VK_SUCCESS) return -1;
            texmem[i].used = off + req.size;
            *mem = texmem[i].mem;
            return 0;
        }
        if (ntexmem < TEXMEM_MAX_BLOCKS) {
            memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = TEXMEM_BLOCK;
            ai.memoryTypeIndex = type;
            if (vkAllocateMemory(dev, &ai, NULL, &texmem[ntexmem].mem) == VK_SUCCESS) {
                i = ntexmem++;
                texmem[i].type = type;
                texmem[i].used = 0;
                if (vkBindImageMemory(dev, img, texmem[i].mem, 0) != VK_SUCCESS) return -1;
                texmem[i].used = req.size;
                *mem = texmem[i].mem;
                return 0;
            }
        }
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev, &ai, NULL, mem) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(dev, img, *mem, 0) != VK_SUCCESS) {
        vkFreeMemory(dev, *mem, NULL);
        *mem = VK_NULL_HANDLE;
        return -1;
    }
    stat_texmem_own++;
    return 0;
}

static int make_tex_image(vk_texture *t, u32 levels) {
    VkImageCreateInfo ii;
    VkImageViewCreateInfo vi;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = t->w;
    ii.extent.height = t->h;
    ii.extent.depth = 1;
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | (levels > 1u ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, NULL, &t->image) != VK_SUCCESS) {
        t->image = VK_NULL_HANDLE;
        return -1;
    }
    if (texmem_bind(t->image, &t->memory) != 0) {
        vkDestroyImage(dev, t->image, NULL);
        t->image = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
        return -1;
    }
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vi, NULL, &t->view) != VK_SUCCESS) {
        vkDestroyImage(dev, t->image, NULL);
        t->image = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

typedef struct {
    u32 index, w, h, generation;
    s32 uploaded_gen;
    u32 rec_seq, hash, bytes;
    u64 key;
} vk_tex_rec;

void ps2_vk_state_save(ps2_state_put put, void *ud) {
    u32 n = ntextures > VK_MAX_TEXTURES ? VK_MAX_TEXTURES : ntextures;
    vk_tex_rec *rec = (vk_tex_rec *)calloc(n ? n : 1u, sizeof *rec);
    u32 i;
    {
        u32 pool[6];
        pool[0] = ntextures;
        pool[1] = (u32)tex_bytes;
        pool[2] = tex_rec_seq;
        pool[3] = (u32)stat_frames;
        pool[4] = rep_ndraws;
        pool[5] = rec_ndraws;
        put(ud, "vk_pool", pool, sizeof pool);
    }
    if (rec) {
        for (i = 0; i < n; i++) {
            rec[i].index = i;
            rec[i].w = textures[i].w;
            rec[i].h = textures[i].h;
            rec[i].generation = textures[i].generation;
            rec[i].uploaded_gen = textures[i].uploaded_gen;
            rec[i].rec_seq = textures[i].rec_seq;
            rec[i].hash = textures[i].hash;
            rec[i].bytes = textures[i].rgba
                         ? textures[i].w * textures[i].h * 4u : 0u;
            rec[i].key = textures[i].key;
        }
        put(ud, "vk_tex", rec, (u64)n * sizeof *rec);
        free(rec);
    }
    if (rep_draws && rep_ndraws)
        put(ud, "vk_draws", rep_draws, (u64)rep_ndraws * sizeof(vk_draw));
    else if (rec_draws && rec_ndraws)
        put(ud, "vk_draws", rec_draws, (u64)rec_ndraws * sizeof(vk_draw));
    {
        u64 budget = 48u << 20, used = 0;
        u32 pass;
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < n; i++) {
                u32 bytes = textures[i].rgba
                          ? textures[i].w * textures[i].h * 4u : 0u;
                char tag[16];
                int big = textures[i].w >= 256u && textures[i].h >= 256u;
                if (!bytes || (pass == 0) != (big != 0)) continue;
                if (used + bytes > budget) continue;
                snprintf(tag, sizeof tag, "vk_px%u", i);
                put(ud, tag, textures[i].rgba, bytes);
                used += bytes;
            }
        }
    }
}

static void barrier_levels(VkCommandBuffer cmd, VkImage img, u32 base, u32 count,
                           VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, img, VK_IMAGE_ASPECT_COLOR_BIT, from, to, srcStage, srcAccess,
                 dstStage, dstAccess);
    b.subresourceRange.baseMipLevel = base;
    b.subresourceRange.levelCount = count;
    barrier_flush(cmd, &b, 1);
}

static void upload_texture_mips(VkCommandBuffer cmd, vk_frame *fr, vk_texture *t) {
    size_t bytes = (size_t)t->w * t->h * 4u;
    u32 gen = __atomic_load_n(&t->generation, __ATOMIC_ACQUIRE);
    VkBufferImageCopy region;
    if (fr->soff + bytes > (16u << 20)) return;
    if (!t->image) {
        if (make_tex_image(t, t->levels) != 0) return;
        t->set = alloc_set_s(t->view, sampler_mip);
        barrier_levels(cmd, t->image, 0, t->levels,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    } else {
        barrier_levels(cmd, t->image, 0, t->levels,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    memcpy((u8 *)fr->sptr + fr->soff, __atomic_load_n(&t->rgba, __ATOMIC_ACQUIRE), bytes);
    memset(&region, 0, sizeof(region));
    region.bufferOffset = fr->soff;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = t->w;
    region.imageExtent.height = t->h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, fr->sbuf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
    fr->soff += (bytes + 255u) & ~(size_t)255u;
    {
        s32 mw = (s32)t->w, mh = (s32)t->h;
        for (u32 lv = 1; lv < t->levels; lv++) {
            VkImageBlit bl;
            barrier_levels(cmd, t->image, lv - 1u, 1,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            memset(&bl, 0, sizeof(bl));
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bl.srcSubresource.mipLevel = lv - 1u;
            bl.srcSubresource.layerCount = 1;
            bl.srcOffsets[1].x = mw;
            bl.srcOffsets[1].y = mh;
            bl.srcOffsets[1].z = 1;
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bl.dstSubresource.mipLevel = lv;
            bl.dstSubresource.layerCount = 1;
            bl.dstOffsets[1].x = mw;
            bl.dstOffsets[1].y = mh;
            bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                           VK_FILTER_LINEAR);
            barrier_levels(cmd, t->image, lv - 1u, 1,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
        barrier_levels(cmd, t->image, t->levels - 1u, 1,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    if (__atomic_load_n(&t->generation, __ATOMIC_ACQUIRE) == gen)
        t->uploaded_gen = (int)gen;
    stat_texuploads++;
}

static void upload_texture(VkCommandBuffer cmd, vk_frame *fr, vk_texture *t) {
    size_t bytes = (size_t)t->w * t->h * 4u;
    VkBufferImageCopy region;
    u32 gen = __atomic_load_n(&t->generation, __ATOMIC_ACQUIRE);
    if (t->levels > 1u) { upload_texture_mips(cmd, fr, t); return; }
    if (fr->soff + bytes > (16u << 20)) {
        static int said;
        if (!said) {
            said = 1;
            ps2_log("vk: staging exhausted at %zu bytes -- texture %ux%u not "
                    "uploaded; draws using it will sample the previous "
                    "contents", (size_t)fr->soff, t->w, t->h);
        }
        return;
    }
    if (!t->image) {
        if (make_tex_image(t, 1u) != 0) return;
        t->set = alloc_set(t->view);
        barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    } else {
        barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    memcpy((u8 *)fr->sptr + fr->soff,
           __atomic_load_n(&t->rgba, __ATOMIC_ACQUIRE), bytes);
    memset(&region, 0, sizeof(region));
    region.bufferOffset = fr->soff;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = t->w;
    region.imageExtent.height = t->h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, fr->sbuf, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    fr->soff += (bytes + 255u) & ~(size_t)255u;
    barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    if (__atomic_load_n(&t->generation, __ATOMIC_ACQUIRE) == gen)
        t->uploaded_gen = (int)gen;
    stat_texuploads++;
}

static void begin_rendering(VkCommandBuffer cmd, vk_target *t, vk_target *zt,
                            int clear, int float_color) {
    VkRenderingAttachmentInfo col, dep;
    VkRenderingInfo ri;
    memset(&col, 0, sizeof(col));
    col.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    col.imageView = float_color ? t->fview : t->cview;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = clear && !float_color ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                       : VK_ATTACHMENT_LOAD_OP_LOAD;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&dep, 0, sizeof(dep));
    dep.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    dep.imageView = zt->dview;
    dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    dep.loadOp = (clear && zt == t) || zt->dclear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                  : VK_ATTACHMENT_LOAD_OP_LOAD;
    zt->dclear = 0;
    dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    dep.clearValue.depthStencil.depth = 0.0f;
    memset(&ri, 0, sizeof(ri));
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = rt_w * rt_scale;
    ri.renderArea.extent.height = rt_h * rt_scale;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &col;
    ri.pDepthAttachment = &dep;
    vkCmdBeginRendering(cmd, &ri);
    rendering_open = 1;
}

static VkDescriptorSet target_snapshot(VkCommandBuffer cmd, vk_target *t) {
    VkImageCopy rgn;
    if (!t->created) return 0;
    if (!t->snap) {
        if (make_image(rt_w * rt_scale, rt_h * rt_scale, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       &t->snap, &t->smem, &t->sview) != 0) {
            t->snap = VK_NULL_HANDLE;
            return 0;
        }
        if (t->sset) rebind_set(t->sset, t->sview);
        else t->sset = alloc_set(t->sview);
        t->slayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->snap_valid = 0;
    }
    if (!t->sset) return 0;
    if (t->snap_valid && t->slayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return t->sset;

    s32 cx0 = 0, cy0 = 0, cx1 = (s32)rt_w, cy1 = (s32)rt_h;
    int partial = t->snap_ever
               && t->slayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (partial) {
        cx0 = t->sdx0 < 0 ? 0 : t->sdx0;
        cy0 = t->sdy0 < 0 ? 0 : t->sdy0;
        cx1 = t->sdx1 > (s32)rt_w ? (s32)rt_w : t->sdx1;
        cy1 = t->sdy1 > (s32)rt_h ? (s32)rt_h : t->sdy1;
        if (cx0 >= cx1 || cy0 >= cy1) {
            t->snap_valid = 1;
            t->sdx0 = (s32)rt_w; t->sdy0 = (s32)rt_h;
            t->sdx1 = 0; t->sdy1 = 0;
            return t->sset;
        }
    }

    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
            t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, t->snap, VK_IMAGE_ASPECT_COLOR_BIT,
            partial ? t->slayout : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    memset(&rgn, 0, sizeof(rgn));
    rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rgn.srcSubresource.layerCount = 1;
    rgn.dstSubresource = rgn.srcSubresource;
    rgn.srcOffset.x = cx0 * (s32)rt_scale;
    rgn.srcOffset.y = cy0 * (s32)rt_scale;
    rgn.dstOffset = rgn.srcOffset;
    rgn.extent.width = (u32)(cx1 - cx0) * rt_scale;
    rgn.extent.height = (u32)(cy1 - cy0) * rt_scale;
    rgn.extent.depth = 1;
    vkCmdCopyImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   t->snap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    barrier(cmd, t->snap, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
    barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    t->slayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t->snap_valid = 1;
    t->snap_ever = 1;
    t->sdx0 = (s32)rt_w; t->sdy0 = (s32)rt_h;
    t->sdx1 = 0; t->sdy1 = 0;
    t->drawn = 1;
    begin_rendering(cmd, t, cur_zt ? cur_zt : t, 0, 0);
    return t->sset;
}

static void select_target(VkCommandBuffer cmd, vk_target *t, vk_target *zt) {
    u32 i;
    VkImageMemoryBarrier2 batch[RT_SLOTS * 2u + 2u];
    u32 nb = 0;
    int zundef;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    for (i = 0; i < RT_SLOTS; i++) {
        vk_target *o = &rts[i];
        if (!o->created) continue;
        if (o != t && o->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier_fill(&batch[nb++], o->color, VK_IMAGE_ASPECT_COLOR_BIT,
                    o->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
            o->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (o != zt && o->dlayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
            barrier_fill(&batch[nb++], o->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                    o->dlayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
            o->dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    if (t->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier_fill(&batch[nb++], t->color, VK_IMAGE_ASPECT_COLOR_BIT,
                t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                    : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    ? VK_ACCESS_2_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    zundef = zt->dlayout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (zt->dlayout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        int was_read = zt->dlayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier_fill(&batch[nb++], zt->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                zt->dlayout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                was_read ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                         : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                was_read ? VK_ACCESS_2_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        zt->dlayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    barrier_flush(cmd, batch, nb);
    if (zt != t && zundef) zt->dclear = 1;
    begin_rendering(cmd, t, zt, !t->drawn, 0);
    t->drawn = 1;
    cur_zt = zt;
}

static int colclip_begin(VkCommandBuffer cmd, vk_target *t, vk_target *zt) {
    u32 iw = rt_w * rt_scale, ih = rt_h * rt_scale;
    VkImageBlit b;
    if (!t->fimg) {
        if (make_image(iw, ih, VK_FORMAT_R32G32B32A32_SFLOAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       &t->fimg, &t->fmem, &t->fview) != 0)
            return -1;
        if (t->fset) rebind_set_s(t->fset, t->fview, sampler_point);
        else t->fset = alloc_set_s(t->fview, sampler_point);
        t->flayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (!t->fset) return -1;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = (s32)iw;
    b.srcOffsets[1].y = (s32)ih;
    b.srcOffsets[1].z = 1;
    b.dstOffsets[1] = b.srcOffsets[1];
    barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
            t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, t->fimg, VK_IMAGE_ASPECT_COLOR_BIT,
            t->flayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdBlitImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   t->fimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &b, VK_FILTER_NEAREST);
    barrier(cmd, t->fimg, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    t->flayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    begin_rendering(cmd, t, zt, 0, 1);
    return 0;
}

static void colclip_resolve(VkCommandBuffer cmd, vk_target *t, vk_target *zt) {
    VkViewport vp;
    VkRect2D sc;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    barrier(cmd, t->fimg, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    t->flayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    begin_rendering(cmd, t, zt, 0, 0);
    memset(&vp, 0, sizeof vp);
    vp.width = (float)(rt_w * rt_scale);
    vp.height = (float)(rt_h * rt_scale);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    memset(&sc, 0, sizeof sc);
    sc.extent.width = rt_w * rt_scale;
    sc.extent.height = rt_h * rt_scale;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, colclip_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            present_layout, 0, 1, &t->fset, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

typedef struct {
    float box[4];
    float sun_z;
    float use_alpha;
    float pad[6];
} rn_vis_push;
typedef struct {
    float inv_size[2];
    float tex_size[2];
    u32 pass, wrap, point, use_vis;
    float pad[4];
} rn_sun_push;

static void native_pass(VkCommandBuffer cmd, vk_frame *fr, const vk_draw *d) {
    vk_target *t, *zs;
    int use_vis = 0;
    VkViewport vp;
    VkRect2D sc;
    if (!rn_native_ok || d->rt >= RT_SLOTS) return;
    t = &rts[d->rt];
    zs = &rts[d->zrt < RT_SLOTS ? d->zrt : d->rt];
    if (!t->created || !zs->created) return;
    if (d->tex == 0xFFFF || d->tex >= ntextures || !textures[d->tex].set) return;

    if (d->nbox[2] > d->nbox[0] && d->nbox[3] > d->nbox[1]
        && zs->dlayout != VK_IMAGE_LAYOUT_UNDEFINED) {
        VkDescriptorSet aset = 0;
        rn_vis_push vpc;
        VkRenderingAttachmentInfo col;
        VkRenderingInfo ri;
        if (zs == t && t->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            aset = target_snapshot(cmd, t);
        else if (zs->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            aset = zs->set;
        if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
        if (zs->dlayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier(cmd, zs->depth, VK_IMAGE_ASPECT_DEPTH_BIT, zs->dlayout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
            zs->dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        barrier(cmd, rn_vis_img, VK_IMAGE_ASPECT_COLOR_BIT, rn_vis_layout_state,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        memset(&col, 0, sizeof col);
        col.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        col.imageView = rn_vis_view;
        col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        memset(&ri, 0, sizeof ri);
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent.width = 1;
        ri.renderArea.extent.height = 1;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &col;
        vkCmdBeginRendering(cmd, &ri);
        memset(&vp, 0, sizeof vp);
        vp.width = 1.0f; vp.height = 1.0f; vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        memset(&sc, 0, sizeof sc);
        sc.extent.width = 1; sc.extent.height = 1;
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rn_vis_pipe);
        {   VkDescriptorSet sets[2];
            sets[0] = zs->dset;
            sets[1] = aset ? aset : white_set;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    rn_vis_layout, 0, 2, sets, 0, NULL);
        }
        memset(&vpc, 0, sizeof vpc);
        for (int i = 0; i < 4; i++) vpc.box[i] = d->nbox[i] * (float)rt_scale;
        vpc.sun_z = d->nsun_z;
        vpc.use_alpha = aset ? 1.0f : 0.0f;
        vkCmdPushConstants(cmd, rn_vis_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof vpc, &vpc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        barrier(cmd, rn_vis_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
        rn_vis_layout_state = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_vis = 1;
        stat_native_vis++;
    } else if (rn_vis_layout_state == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        use_vis = 1;
        stat_native_vis_kept++;
    } else {
        stat_native_vis_none++;
        return;
    }

    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    if (t->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    {
        VkRenderingAttachmentInfo col;
        VkRenderingInfo ri;
        rn_sun_push spc;
        memset(&col, 0, sizeof col);
        col.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        col.imageView = t->cview;
        col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col.loadOp = t->drawn ? VK_ATTACHMENT_LOAD_OP_LOAD
                              : VK_ATTACHMENT_LOAD_OP_CLEAR;
        col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        memset(&ri, 0, sizeof ri);
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent.width = rt_w * rt_scale;
        ri.renderArea.extent.height = rt_h * rt_scale;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &col;
        vkCmdBeginRendering(cmd, &ri);
        t->drawn = 1;
        memset(&vp, 0, sizeof vp);
        vp.width = (float)(rt_w * rt_scale);
        vp.height = (float)(rt_h * rt_scale);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        {
            s32 x0 = d->scissor[0] < 0 ? 0 : d->scissor[0];
            s32 y0 = d->scissor[2] < 0 ? 0 : d->scissor[2];
            s32 x1 = d->scissor[1] + 1, y1 = d->scissor[3] + 1;
            if (x0 > (s32)rt_w - 1) x0 = (s32)rt_w - 1;
            if (y0 > (s32)rt_h - 1) y0 = (s32)rt_h - 1;
            if (x1 > (s32)rt_w) x1 = (s32)rt_w;
            if (y1 > (s32)rt_h) y1 = (s32)rt_h;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            sc.offset.x = x0 * (s32)rt_scale;
            sc.offset.y = y0 * (s32)rt_scale;
            sc.extent.width = (u32)(x1 - x0) * rt_scale;
            sc.extent.height = (u32)(y1 - y0) * rt_scale;
            vkCmdSetScissor(cmd, 0, 1, &sc);
            t->snap_valid = 0;
            if (x0 < t->sdx0) t->sdx0 = x0;
            if (y0 < t->sdy0) t->sdy0 = y0;
            if (x1 > t->sdx1) t->sdx1 = x1;
            if (y1 > t->sdy1) t->sdy1 = y1;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rn_sun_pipe);
        {   VkDescriptorSet sets[2];
            VkDeviceSize zero = 0;
            sets[0] = textures[d->tex].set;
            sets[1] = use_vis ? rn_vis_set : white_set;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    rn_sun_layout, 0, 2, sets, 0, NULL);
            vkCmdBindVertexBuffers(cmd, 0, 1, &fr->vbuf, &zero);
        }
        memset(&spc, 0, sizeof spc);
        spc.inv_size[0] = 2.0f / (float)rt_w;
        spc.inv_size[1] = 2.0f / (float)rt_h;
        spc.tex_size[0] = d->tex_w > 0.0f ? d->tex_w : 1.0f;
        spc.tex_size[1] = d->tex_h > 0.0f ? d->tex_h : 1.0f;
        spc.pass = d->native;
        spc.wrap = d->wrap;
        spc.point = d->npoint;
        spc.use_vis = (u32)use_vis;
        vkCmdPushConstants(cmd, rn_sun_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof spc, &spc);
        vkCmdDraw(cmd, d->count, 1, d->first, 0);
        vkCmdEndRendering(cmd);
        stat_native_draws++;
    }
}

static void snap_sprite_rows(ps2_vk_vertex *out) {
    for (u32 i = 0; i < rep_ndraws; i++) {
        const vk_draw *d = &rep_draws[i];
        if (d->topology != PS2_VK_TRIANGLES || d->native == PS2_VK_NATIVE_MESH) continue;
        if (d->first > rep_nverts || d->count > rep_nverts - d->first) continue;
        for (u32 k = d->first; k + 3u <= d->first + d->count; k += 3u) {
            const ps2_vk_vertex *v = &rep_verts[k];
            float y0, y1, t0, t1, p0, p1, dt;
            if (!(v[0].round_uv & PS2_VK_RUV_SPRITE)) continue;
            y0 = y1 = v[0].y;
            t0 = t1 = v[0].t;
            for (int j = 1; j < 3; j++) {
                if (v[j].y < y0) { y0 = v[j].y; t0 = v[j].t; }
                if (v[j].y > y1) { y1 = v[j].y; t1 = v[j].t; }
            }
            p0 = ceilf(y0 - 0.5f);
            p1 = ceilf(y1 - 0.5f);
            if (!(y1 > y0) || !(p1 > p0)) continue;
            dt = (t1 - t0) / (y1 - y0);
            for (int j = 0; j < 3; j++) {
                int top = v[j].y == y0;
                out[k + j].y = top ? p0 : p1;
                out[k + j].t = top ? t0 + (p0 - y0) * dt : t1 + (p1 - y1) * dt;
            }
        }
    }
}

typedef struct {
    vk_draw *d;
    ps2_vk_vertex *v;
    u32 nd, nv;
    int overflow;
} ov_list;
static ov_list ov_lists[2];
static int ov_accum;
static u8 *ov_eligible;
static int ov_on = -1;
static int ov_list_log = -1;
static vk_target ov_comp;
static u32 ov_k, ov_cw, ov_ch;
static int ov_show;
static int ov_seen;
static int ov_presented;
static u64 stat_ov_frames, stat_ov_draws, stat_ov_inplace, stat_ov_rgbreads;

static int ov_enabled(void) {
    if (ov_on < 0) {
        const char *e = getenv("PS2_RN_2D_OVERLAY");
        ov_on = !(e && *e == '0');
        for (int i = 0; ov_on && i < 2; i++) {
            ov_lists[i].d = (vk_draw *)malloc(sizeof(vk_draw) * OV_MAX_DRAWS);
            ov_lists[i].v = (ps2_vk_vertex *)malloc(sizeof(ps2_vk_vertex) * OV_MAX_VERTS);
            if (!ov_lists[i].d || !ov_lists[i].v) ov_on = 0;
        }
        ov_eligible = ov_on ? (u8 *)malloc(VK_MAX_DRAWS) : NULL;
        if (!ov_eligible) ov_on = 0;
    }
    return ov_on;
}

static int ov_is_composite(const vk_draw *d) {
    return d->tex_rt == 1u && !d->tex_depth && d->rt != 0u && d->rt == present_rt;
}

#define OV_CELL 16
#define OV_CW   (RT_MAX / OV_CELL)
#define OV_CH   (RT_MAX / OV_CELL)
static u8 ov_grid[OV_CH][OV_CW];
#define OV_HOLD 30u
static u64 ov_hold[OV_CH][OV_CW];

static int ov_held(s32 x0, s32 y0, s32 x1, s32 y1) {
    for (s32 y = y0; y <= y1; y++)
        for (s32 x = x0; x <= x1; x++)
            if (ov_hold[y][x] && ov_hold[y][x] + OV_HOLD > stat_frames) return 1;
    return 0;
}

static void ov_hold_mark(s32 x0, s32 y0, s32 x1, s32 y1) {
    for (s32 y = y0; y <= y1; y++)
        for (s32 x = x0; x <= x1; x++)
            ov_hold[y][x] = stat_frames ? stat_frames : 1u;
}

static int ov_additive(const vk_draw *d) {
    u32 b = d->blend;
    return (b & 0x100u) && (b & 3u) == 0u && ((b >> 2) & 3u) == 2u
        && ((b >> 6) & 3u) == 1u;
}

static const u8 *screen_mesh_rec(const vk_draw *d, u32 k) {
    u32 ix;
    if (d->native != PS2_VK_NATIVE_MESH || !rep_mesh || d->mblock >= rep_mesh->nblocks
        || rep_mesh->blocks[d->mblock].prog != 5u || d->first + k >= rep_mesh->nidx)
        return NULL;
    ix = rep_mesh->idx[d->first + k] + d->mvbase;
    return ix < rep_mesh->nrecs ? rep_mesh->recs + (size_t)ix * MESH_REC_BYTES : NULL;
}

static void ov_bbox(const vk_draw *d, s32 *x0, s32 *y0, s32 *x1, s32 *y1) {
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    if (d->native == PS2_VK_NATIVE_MESH && !screen_mesh_rec(d, 0)) {
        bx0 = by0 = -1e9f; bx1 = by1 = 1e9f;
    }
    for (u32 k = 0; k < d->count && d->native == PS2_VK_NATIVE_MESH; k++) {
        const u8 *r = screen_mesh_rec(d, k);
        float p[2];
        if (!r) break;
        memcpy(p, r, 8);
        if (p[0] < bx0) bx0 = p[0];
        if (p[0] > bx1) bx1 = p[0];
        if (p[1] < by0) by0 = p[1];
        if (p[1] > by1) by1 = p[1];
    }
    for (u32 k = 0; k < d->count && d->native != PS2_VK_NATIVE_MESH; k++) {
        const ps2_vk_vertex *v = &rep_verts[d->first + k];
        if (v->x < bx0) bx0 = v->x;
        if (v->x > bx1) bx1 = v->x;
        if (v->y < by0) by0 = v->y;
        if (v->y > by1) by1 = v->y;
    }
    bx0 -= 1.0f; by0 -= 1.0f; bx1 += 1.0f; by1 += 1.0f;
    if (bx0 < (float)d->scissor[0]) bx0 = (float)d->scissor[0];
    if (by0 < (float)d->scissor[2]) by0 = (float)d->scissor[2];
    if (bx1 > (float)d->scissor[1] + 1.0f) bx1 = (float)d->scissor[1] + 1.0f;
    if (by1 > (float)d->scissor[3] + 1.0f) by1 = (float)d->scissor[3] + 1.0f;
    *x0 = bx0 < 0.0f ? 0 : (s32)bx0 / OV_CELL;
    *y0 = by0 < 0.0f ? 0 : (s32)by0 / OV_CELL;
    *x1 = bx1 < 0.0f ? -1 : (s32)bx1 / OV_CELL;
    *y1 = by1 < 0.0f ? -1 : (s32)by1 / OV_CELL;
    if (*x1 >= OV_CW) *x1 = OV_CW - 1;
    if (*y1 >= OV_CH) *y1 = OV_CH - 1;
}

static int ov_colourless(const vk_draw *d) {
    if (!((d->depth >> 3) & 1u)) return 1;
    if ((d->blend & 0x1FFu) == 0x144u && d->tex == 0xFFFF && !d->tex_rt
        && d->native != PS2_VK_NATIVE_MESH) {
        for (u32 k = 0; k < d->count; k++)
            if (rep_verts[d->first + k].a != 0.0f) return 0;
        return 1;
    }
    if ((d->blend & 0x1FFu) == 0x144u && d->tex == 0xFFFF && !d->tex_rt
        && screen_mesh_rec(d, 0)) {
        for (u32 k = 0; k < d->count; k++) {
            const u8 *r = screen_mesh_rec(d, k);
            s32 a;
            if (!r) return 0;
            memcpy(&a, r + 32 + 12, 4);
            if (a != 0) return 0;
        }
        return 1;
    }
    return 0;
}

static void ov_mark(s32 x0, s32 y0, s32 x1, s32 y1, int additive) {
    for (s32 y = y0; y <= y1; y++)
        for (s32 x = x0; x <= x1; x++)
            ov_grid[y][x] |= additive ? 2u : 1u;
}

static int ov_hit(s32 x0, s32 y0, s32 x1, s32 y1, int additive) {
    u8 mask = additive ? 1u : 3u;
    for (s32 y = y0; y <= y1; y++)
        for (s32 x = x0; x <= x1; x++)
            if (ov_grid[y][x] & mask) return 1;
    return 0;
}

static int ov_safe(const vk_draw *d) {
    u32 dk = d->depth;
    if (d->native && d->native != PS2_VK_NATIVE_2D && !screen_mesh_rec(d, 0)) return 0;
    if (d->tex_self || d->tex_rt == 1u || d->date) return 0;
    if ((d->blend & 0x100u) && ((d->blend >> 4) & 3u) == 1u) return 0;
    if ((dk >> 9) & 3u) return 0;
    if ((dk & 3u) != 1u) return 0;
    return 1;
}

static int ov_reads_slot0_rgb(const vk_draw *d) {
    u32 tfx = d->frag_flags & 3u;
    if (d->tex_rt != 1u || d->tex_depth) return 0;
    if (!((d->depth >> 3) & 1u)) return 0;
    if (((d->depth >> 5) & 7u) == 7u) return 0;
    if (d->native == PS2_VK_NATIVE_MESH) {
        if (!screen_mesh_rec(d, 0) || tfx == 1u || ((d->frag_flags >> 24) & 1u)) return 1;
        for (u32 k = 0; k < d->count; k++) {
            const u8 *r = screen_mesh_rec(d, k);
            s32 c[3];
            if (!r) return 1;
            memcpy(c, r + 32, 12);
            if (c[0] || c[1] || c[2]) return 1;
        }
        return 0;
    }
    if (tfx == 1u || ((d->frag_flags >> 24) & 1u)) return 1;
    for (u32 k = 0; k < d->count; k++) {
        const ps2_vk_vertex *v = &rep_verts[d->first + k];
        if (v->r != 0.0f || v->g != 0.0f || v->b != 0.0f) return 1;
    }
    return 0;
}

static void ov_scan(void) {
    static int log_left = -1;
    u32 seg_end = rep_ndraws;
    if (log_left < 0) {
        const char *e = getenv("PS2_RN_OV_LOG");
        log_left = e ? atoi(e) : 0;
    }
    memset(ov_eligible, 0, rep_ndraws);
    while (seg_end > 0) {
        u32 seg_start = seg_end, first2d = ~0u;
        int tail = 1;
        while (seg_start > 0 && !ov_is_composite(&rep_draws[seg_start - 1]))
            seg_start--;
        for (u32 i = seg_start; i < seg_end; i++)
            if (rep_draws[i].native == PS2_VK_NATIVE_2D && rep_draws[i].rt == 0u) {
                first2d = i;
                break;
            }
        if (first2d != ~0u) {
            ov_seen = 1;
            memset(ov_grid, 0, sizeof ov_grid);
            for (u32 i = seg_end; i-- > first2d;) {
                const vk_draw *d = &rep_draws[i];
                int is2d = d->native == PS2_VK_NATIVE_2D;
                s32 x0, y0, x1, y1;
                if (d->rt != 0u) {
                    if (ov_reads_slot0_rgb(d)) {
                        tail = 0;
                        memset(ov_grid, 1, sizeof ov_grid);
                        stat_ov_rgbreads++;
                    }
                    continue;
                }
                if (!is2d && ov_colourless(d)) continue;
                ov_bbox(d, &x0, &y0, &x1, &y1);
                if (tail && ov_safe(d) && !(is2d && ov_held(x0, y0, x1, y1))) {
                    ov_eligible[i] = 1;
                    continue;
                }
                if (tail && log_left > 0) {
                    ps2_log("ov: frame %llu tail ends at draw %u of %u: emitter %u "
                            "native=%u texrt=%u self=%u date=%u blend=%03X "
                            "depth=%03X", (unsigned long long)stat_frames, i,
                            rep_ndraws, d->emitter, d->native, d->tex_rt,
                            d->tex_self, d->date, d->blend, d->depth);
                    log_left--;
                }
                tail = 0;
                if (is2d && ov_safe(d) && !ov_held(x0, y0, x1, y1)
                    && !ov_hit(x0, y0, x1, y1, ov_additive(d))) {
                    ov_eligible[i] = 1;
                    continue;
                }
                if (is2d && (!ov_safe(d) || ov_hit(x0, y0, x1, y1, ov_additive(d))))
                    ov_hold_mark(x0, y0, x1, y1);
                if (is2d) stat_ov_inplace++;
                if (d->tex_rt == 1u) memset(ov_grid, 1, sizeof ov_grid);
                else ov_mark(x0, y0, x1, y1, ov_additive(d));
            }
        }
        seg_end = seg_start ? seg_start - 1u : 0u;
    }
    {
        static int init;
        static s32 rx0, ry0, rx1, ry1;
        static int frames_left;
        if (!init) {
            const char *e = getenv("PS2_RN_OV_REGION");
            init = 1;
            if (e && sscanf(e, "%d,%d,%d,%d", &rx0, &ry0, &rx1, &ry1) == 4) frames_left = 600;
        }
        if (frames_left > 0) {
            u32 lifted = 0, inplace = 0;
            frames_left--;
            for (u32 i = 0; i < rep_ndraws; i++) {
                const vk_draw *d = &rep_draws[i];
                float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                if (d->native != PS2_VK_NATIVE_2D || d->rt != 0u) continue;
                for (u32 k = 0; k < d->count; k++) {
                    const ps2_vk_vertex *vv = &rep_verts[d->first + k];
                    if (vv->x < bx0) bx0 = vv->x;
                    if (vv->x > bx1) bx1 = vv->x;
                    if (vv->y < by0) by0 = vv->y;
                    if (vv->y > by1) by1 = vv->y;
                }
                if (bx1 < (float)rx0 || bx0 > (float)rx1 || by1 < (float)ry0 || by0 > (float)ry1)
                    continue;
                if (ov_eligible[i]) lifted++;
                else inplace++;
            }
            ps2_log("ov-region: frame %llu lifted %u in place %u",
                    (unsigned long long)stat_frames, lifted, inplace);
        }
    }
}

static void ov_add(const vk_draw *d) {
    ov_list *l = &ov_lists[ov_accum];
    if (l->nd >= OV_MAX_DRAWS || l->nv + d->count > OV_MAX_VERTS) {
        l->overflow = 1;
        return;
    }
    l->d[l->nd] = *d;
    l->d[l->nd].first = l->nv;
    if (d->native == PS2_VK_NATIVE_MESH) {
        for (u32 k = 0; k < d->count; k++) {
            const u8 *r = screen_mesh_rec(d, k);
            ps2_vk_vertex *v = &l->v[l->nv + k];
            float p[4], uv[4];
            s32 nrm[4], c[4];
            if (!r) { l->overflow = 1; return; }
            memcpy(p, r, 16); memcpy(nrm, r + 16, 16);
            memcpy(c, r + 32, 16); memcpy(uv, r + 48, 16);
            v->x = p[0]; v->y = p[1]; v->z = p[2];
            v->r = (float)c[0]; v->g = (float)c[1]; v->b = (float)c[2]; v->a = (float)c[3];
            v->s = uv[0]; v->t = uv[1]; v->q = uv[2]; v->fog = uv[3];
            v->round_uv = (u32)nrm[0];
        }
        l->d[l->nd].native = PS2_VK_NATIVE_2D;
        l->d[l->nd].mcull = 0;
    } else {
        memcpy(l->v + l->nv, rep_verts + d->first, sizeof(ps2_vk_vertex) * d->count);
    }
    l->nv += d->count;
    l->nd++;
}

static void ov_promote(void) {
    ov_accum = 1 - ov_accum;
    ov_lists[ov_accum].nd = 0;
    ov_lists[ov_accum].nv = 0;
    ov_lists[ov_accum].overflow = 0;
}

static void ov_prepare(void) {
    static int env_k = -1;
    u32 k, big = rt_w > rt_h ? rt_w : rt_h, lim;
    if (env_k < 0) {
        const char *e = getenv("PS2_RN_2D_SCALE");
        env_k = e && atoi(e) > 0 ? atoi(e) : 0;
    }
    ov_show = 0;
    if (!ov_enabled() || !ov_seen || !present_h) return;
    k = env_k > 0 ? (u32)env_k
                  : (swap_extent.height + present_h - 1u) / present_h;
    if (k < rt_scale) k = rt_scale;
    lim = max_image_dim / (big ? big : 1u);
    if (k > lim) k = lim;
    if (k > RT_SCALE_MAX) k = RT_SCALE_MAX;
    if (k < 1u) k = 1u;
    if (!ov_comp.created || ov_k != k || ov_cw != rt_w || ov_ch != rt_h) {
        vkDeviceWaitIdle(dev);
        destroy_target_images(&ov_comp);
        if (make_image(rt_w * k, rt_h * k, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT,
                       &ov_comp.color, &ov_comp.cmem, &ov_comp.cview) != 0
            || make_image(rt_w * k, rt_h * k, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT,
                          &ov_comp.depth, &ov_comp.dmem, &ov_comp.dview) != 0) {
            destroy_target_images(&ov_comp);
            ov_on = 0;
            ps2_log("vk: the 2D overlay target (%ux%u) could not be made; "
                    "native 2D is drawn in place", rt_w * k, rt_h * k);
            return;
        }
        if (ov_comp.set) rebind_set(ov_comp.set, ov_comp.cview);
        else ov_comp.set = alloc_set(ov_comp.cview);
        ov_comp.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        ov_comp.dlayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ov_comp.created = 1;
        ov_k = k; ov_cw = rt_w; ov_ch = rt_h;
        ps2_log("vk: 2D overlay composed at %ux (%ux%u)", k, rt_w * k, rt_h * k);
    }
    ov_show = ov_comp.set != VK_NULL_HANDLE;
}

static void ov_compose(VkCommandBuffer cmd, vk_frame *fr) {
    ov_list *r = &ov_lists[1 - ov_accum];
    vk_target *src = &rts[present_rt < RT_SLOTS && rts[present_rt].created
                          ? present_rt : 0];
    u32 k = ov_k, iw = rt_w * k, ih = rt_h * k;
    float line_w = (float)k, point_sz = (float)k;
    VkImageBlit b;
    VkViewport vp;
    VkRenderingAttachmentInfo col, dep;
    VkRenderingInfo ri;
    VkDeviceSize zero = 0;
    VkPipeline bound = VK_NULL_HANDLE;
    if (!ov_show || !r->nd || !src->created || src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        ov_show = 0;
        return;
    }
    if (line_w > max_line_width) line_w = max_line_width;
    if (point_sz > max_point_size) point_sz = max_point_size;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }

    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = (s32)(rt_w * rt_scale);
    b.srcOffsets[1].y = (s32)(rt_h * rt_scale);
    b.srcOffsets[1].z = 1;
    b.dstOffsets[1].x = (s32)iw;
    b.dstOffsets[1].y = (s32)ih;
    b.dstOffsets[1].z = 1;
    barrier(cmd, src->color, VK_IMAGE_ASPECT_COLOR_BIT, src->layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, ov_comp.color, VK_IMAGE_ASPECT_COLOR_BIT, ov_comp.layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdBlitImage(cmd, src->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   ov_comp.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   k == rt_scale ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier(cmd, src->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src->layout,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    barrier(cmd, ov_comp.color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    ov_comp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier(cmd, ov_comp.depth, VK_IMAGE_ASPECT_DEPTH_BIT, ov_comp.dlayout,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    ov_comp.dlayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    memset(&col, 0, sizeof col);
    col.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    col.imageView = ov_comp.cview;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    dep = col;
    dep.imageView = ov_comp.dview;
    dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    dep.clearValue.depthStencil.depth = 0.0f;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = iw;
    ri.renderArea.extent.height = ih;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &col;
    ri.pDepthAttachment = &dep;
    vkCmdBeginRendering(cmd, &ri);

    memcpy(fr->ovptr, r->v, sizeof(ps2_vk_vertex) * r->nv);
    {   static int snap = -1;
        if (snap < 0) {
            const char *e = getenv("PS2_OV_SNAP");
            snap = !(e && *e == '0');
        }
        if (!snap) k = 1u;
    }
    if (k > 1u) {
        ps2_vk_vertex *out = (ps2_vk_vertex *)fr->ovptr;
        for (u32 i = 0; i < r->nd; i++) {
            const vk_draw *d = &r->d[i];
            if (d->topology != PS2_VK_TRIANGLES) continue;
            for (u32 j = d->first; j + 3u <= d->first + d->count; j += 3u) {
                const ps2_vk_vertex *v = &r->v[j];
                float y0, y1, t0, t1, p0, p1, dt;
                if (!(v[0].round_uv & PS2_VK_RUV_SPRITE)) continue;
                y0 = y1 = v[0].y;
                t0 = t1 = v[0].t;
                for (int m = 1; m < 3; m++) {
                    if (v[m].y < y0) { y0 = v[m].y; t0 = v[m].t; }
                    if (v[m].y > y1) { y1 = v[m].y; t1 = v[m].t; }
                }
                p0 = ceilf(y0 - 0.5f);
                p1 = ceilf(y1 - 0.5f);
                if (!(y1 > y0) || !(p1 > p0)) continue;
                dt = (t1 - t0) / (y1 - y0);
                for (int m = 0; m < 3; m++) {
                    int top = v[m].y == y0;
                    out[j + m].y = top ? p0 : p1;
                    out[j + m].t = top ? t0 + (p0 - y0) * dt : t1 + (p1 - y1) * dt;
                }
            }
        }
    }
    k = ov_k;
    vkCmdBindVertexBuffers(cmd, 0, 1, &fr->ovbuf, &zero);
    memset(&vp, 0, sizeof vp);
    vp.width = (float)iw;
    vp.height = (float)ih;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    for (u32 i = 0; i < r->nd; i++) {
        const vk_draw *d = &r->d[i];
        VkPipeline pipe = get_pipeline(d->topology, d->blend, d->depth);
        VkDescriptorSet sets[2];
        VkRect2D sc;
        gs_push pc;
        float bc[4];
        if (!pipe) continue;
        if (pipe != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdSetLineWidth(cmd, line_w);
            bound = pipe;
        }
        sets[0] = d->tex != 0xFFFF && d->tex < ntextures && textures[d->tex].set
                ? textures[d->tex].set : white_set;
        if (d->tex_rt && d->tex_rt <= RT_SLOTS) {
            vk_target *t = &rts[d->tex_rt - 1];
            VkImageLayout l = d->tex_depth ? t->dlayout : t->layout;
            if (t->created && l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                sets[0] = d->tex_depth ? t->dset : t->set;
        }
        sets[1] = white_set;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gs_layout,
                                0, 2, sets, 0, NULL);
        bc[0] = bc[1] = bc[2] = bc[3] = (float)d->fix / 128.0f;
        vkCmdSetBlendConstants(cmd, bc);
        {
            s32 x0 = d->scissor[0] < 0 ? 0 : d->scissor[0];
            s32 y0 = d->scissor[2] < 0 ? 0 : d->scissor[2];
            s32 x1 = d->scissor[1] + 1, y1 = d->scissor[3] + 1;
            if (x0 > (s32)rt_w - 1) x0 = (s32)rt_w - 1;
            if (y0 > (s32)rt_h - 1) y0 = (s32)rt_h - 1;
            if (x1 > (s32)rt_w) x1 = (s32)rt_w;
            if (y1 > (s32)rt_h) y1 = (s32)rt_h;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            sc.offset.x = x0 * (s32)k;
            sc.offset.y = y0 * (s32)k;
            sc.extent.width = (u32)(x1 - x0) * k;
            sc.extent.height = (u32)(y1 - y0) * k;
            vkCmdSetScissor(cmd, 0, 1, &sc);
        }
        memset(&pc, 0, sizeof pc);
        pc.scale = (float)k;
        pc.point_size = point_sz;
        pc.inv_size[0] = 2.0f / (float)rt_w;
        pc.inv_size[1] = 2.0f / (float)rt_h;
        pc.tex_size[0] = d->tex_w > 0.0f ? d->tex_w : 1.0f;
        pc.tex_size[1] = d->tex_h > 0.0f ? d->tex_h : 1.0f;
        if (d->tex_rt) {
            pc.img_size[0] = (float)rt_w;
            pc.img_size[1] = (float)rt_h;
        } else if (d->tex != 0xFFFF && d->tex < ntextures) {
            pc.img_size[0] = (float)textures[d->tex].w;
            pc.img_size[1] = (float)textures[d->tex].h;
        } else {
            pc.img_size[0] = pc.img_size[1] = 1.0f;
        }
        pc.tex_scale = d->tex_rt ? (float)rt_scale : 1.0f;
        pc.flags[0] = d->frag_flags;
        pc.flags[1] = (d->tex != 0xFFFF || d->tex_rt) ? 1u : 0u;
        pc.flags[2] = d->wrap;
        pc.flags[3] = debug_solid;
        pc.clamp_uv[0] = d->minu;
        pc.clamp_uv[1] = d->maxu;
        pc.clamp_uv[2] = d->minv;
        pc.clamp_uv[3] = d->maxv;
        pc.fogcol[0] = (float)((d->fogcol >> 0) & 0xFF) / 255.0f;
        pc.fogcol[1] = (float)((d->fogcol >> 8) & 0xFF) / 255.0f;
        pc.fogcol[2] = (float)((d->fogcol >> 16) & 0xFF) / 255.0f;
        pc.fogcol[3] = 1.0f;
        vkCmdPushConstants(cmd, gs_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof pc, &pc);
        vkCmdDraw(cmd, d->count, 1, d->first, 0);
        stat_ov_draws++;
    }
    vkCmdEndRendering(cmd);
    barrier(cmd, ov_comp.color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    ov_comp.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    stat_ov_frames++;
}

static void render_list(VkCommandBuffer cmd, vk_frame *fr) {
    VkViewport vp;
    u32 i;
    size_t vbytes = (size_t)rep_nverts * sizeof(ps2_vk_vertex);
    u32 cur_fb_w = 0;
    int cur_rt = -1, cur_zrt = -1;
    int clip_open = 0;
    VkPipeline bound_pipe = VK_NULL_HANDLE;
    VkDescriptorSet bound_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkRect2D bound_sc = {{0, 0}, {0, 0}};
    gs_push bound_pc;
    int have_sc = 0, have_pc = 0;
    u32 bound_fix = ~0u;
    int vb = 0, mset_ok = 0, ib_ok = 0;
    float line_w = (float)rt_scale, point_sz = (float)rt_scale;
    if (line_w > max_line_width) line_w = max_line_width;
    if (point_sz > max_point_size) point_sz = max_point_size;

    if (rep_nverts) {
        memcpy(fr->vptr, rep_verts, vbytes);
        if (rt_scale > 1u) snap_sprite_rows((ps2_vk_vertex *)fr->vptr);
    }
    if (mesh_ok && rep_mesh && rep_mesh->nblocks) {
        memcpy(fr->mvptr, rep_mesh->recs, (size_t)rep_mesh->nrecs * MESH_REC_BYTES);
        memcpy(fr->miptr, rep_mesh->idx, (size_t)rep_mesh->nidx * sizeof(u32));
        memcpy(fr->mcptr, rep_mesh->blocks,
               (size_t)rep_mesh->nblocks * sizeof(ps2_vk_mesh_block));
    }
    fr->soff = 0;

    {   u32 lim = want_draw_limit();
        if (lim) {
            u32 keep = 0;
            while (keep < rep_ndraws && rep_draws[keep].seq < lim) keep++;
            rep_ndraws = keep;
        }
    }
    for (i = 0; i < rep_ndraws; i++) {
        u16 ti = rep_draws[i].tex;
        if (ti != 0xFFFF && ti < ntextures) {
            vk_texture *t = &textures[ti];
            if (t->rgba && t->uploaded_gen != (int)t->generation)
                upload_texture(cmd, fr, t);
        }
        if (rep_draws[i].rt < RT_SLOTS) create_target(&rts[rep_draws[i].rt]);
        if (rep_draws[i].zrt < RT_SLOTS) create_target(&rts[rep_draws[i].zrt]);
        if (rep_draws[i].tex_rt && rep_draws[i].tex_rt <= RT_SLOTS)
            create_target(&rts[rep_draws[i].tex_rt - 1]);
    }

    if (ov_enabled()) ov_scan();

    select_target(cmd, &rts[0], &rts[0]);
    cur_rt = 0;
    cur_zrt = 0;

    vb = 0;

    for (i = 0; i < rep_ndraws; i++) {
        vk_draw *d = &rep_draws[i];
        VkPipeline pipe;
        VkDescriptorSet set = white_set;
        VkRect2D sc;
        gs_push pc;
        float fixa = (float)d->fix / 128.0f;
        float bc[4];
        u32 zrt = d->zrt < RT_SLOTS && rts[d->zrt].created ? d->zrt : d->rt;
        u32 dkey = d->depth;
        int want_clip = ((dkey >> 9) & 1u) && colclip_ok && !d->tex_self && !d->date;

        if (ov_on > 0) {
            if (ov_eligible[i]) {
                ov_add(d);
                dkey &= ~(1u << 3);
                if (!((dkey >> 4) & 1u) && !((dkey >> 2) & 1u)) continue;
            }
            if (ov_is_composite(d)) {
                if (vk_env_flag("PS2_RN_OV_LIST", &ov_list_log))
                    ps2_log("ov-list: frame %llu composite at draw %u of %u into rt%u (present_rt %u), promoting %u draws",
                            (unsigned long long)stat_frames, i, rep_ndraws, d->rt, present_rt, ov_lists[ov_accum].nd);
                ov_promote();
            }
            else if (d->tex_rt == 1u && d->rt != 0u
                     && vk_env_flag("PS2_RN_OV_LIST", &ov_list_log))
                ps2_log("ov-list: frame %llu draw %u reads slot 0 into rt%u (present_rt %u) -- not a composite",
                        (unsigned long long)stat_frames, i, d->rt, present_rt);
        }

        if (d->native && d->native != PS2_VK_NATIVE_2D && d->native != PS2_VK_NATIVE_MESH) {
            if (clip_open) {
                colclip_resolve(cmd, &rts[cur_rt], &rts[cur_zrt]);
                clip_open = 0;
            }
            native_pass(cmd, fr, d);
            if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
            cur_rt = -1;
            cur_zrt = -1;
            bound_pipe = VK_NULL_HANDLE;
            bound_sets[0] = bound_sets[1] = VK_NULL_HANDLE;
            mset_ok = 0;
            have_sc = 0;
            have_pc = 0;
            bound_fix = ~0u;
            cur_fb_w = 0;
            vb = 0;
            continue;
        }

        if (clip_open && (!want_clip || (int)d->rt != cur_rt || (int)zrt != cur_zrt)) {
            colclip_resolve(cmd, &rts[cur_rt], &rts[cur_zrt]);
            clip_open = 0;
            bound_pipe = VK_NULL_HANDLE;
            bound_sets[0] = bound_sets[1] = VK_NULL_HANDLE;
            mset_ok = 0;
            have_sc = 0;
            have_pc = 0;
            bound_fix = ~0u;
            cur_fb_w = 0;
            vb = 0;
        }
        if (d->rt < RT_SLOTS && rts[d->rt].created
            && ((int)d->rt != cur_rt || (int)zrt != cur_zrt)) {
            select_target(cmd, &rts[d->rt], &rts[zrt]);
            cur_rt = (int)d->rt;
            cur_zrt = (int)zrt;
            cur_fb_w = 0;
            vb = 0;
        }
        if (want_clip && !clip_open && (int)d->rt == cur_rt
            && colclip_begin(cmd, &rts[cur_rt], &rts[cur_zrt]) == 0) {
            clip_open = 1;
            cur_fb_w = 0;
            have_sc = 0;
            bound_fix = ~0u;
            vb = 0;
        }
        if (!clip_open) dkey &= ~(1u << 9);
        pipe = d->native == PS2_VK_NATIVE_MESH
             ? get_pipeline_ex(d->topology, d->blend, dkey,
                               1u | ((d->mcull & 2u) ? 2u : 0u), d->mcull & 1u)
             : get_pipeline(d->topology, d->blend, dkey);
        if (!pipe) continue;
        {
            VkDescriptorSet rset = 0;
            if (d->tex_self && d->rt < RT_SLOTS && (int)d->rt == cur_rt
                && d->tex_rt == (u16)(d->rt + 1u) && !d->tex_depth) {
                if (i > 0 && (rep_draws[i - 1].frag_flags != d->frag_flags
                    || rep_draws[i - 1].blend != d->blend
                    || rep_draws[i - 1].wrap != d->wrap))
                    rts[d->rt].snap_valid = 0;
                int fresh = !rts[d->rt].snap_valid;
                rset = target_snapshot(cmd, &rts[d->rt]);
                if (rset) stat_selfreads++;
                if (fresh && rset) {
                    cur_fb_w = 0;
                    vb = 0;
                }
            } else if (d->tex_rt && d->tex_rt <= RT_SLOTS) {
                vk_target *src = &rts[d->tex_rt - 1];
                VkImageLayout rl = d->tex_depth ? src->dlayout : src->layout;
                if (rl == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                    rset = d->tex_depth ? src->dset : src->set;
            }
            if (d->rt < RT_SLOTS) {
                vk_target *inv = &rts[d->rt];
                if (!d->tex_self) inv->snap_valid = 0;
                if (d->scissor[0] < inv->sdx0) inv->sdx0 = d->scissor[0];
                if (d->scissor[2] < inv->sdy0) inv->sdy0 = d->scissor[2];
                if (d->scissor[1] + 1 > inv->sdx1) inv->sdx1 = d->scissor[1] + 1;
                if (d->scissor[3] + 1 > inv->sdy1) inv->sdy1 = d->scissor[3] + 1;
            }
            if (rset) set = rset;
            else if (d->tex != 0xFFFF && d->tex < ntextures
                     && textures[d->tex].set)
                set = textures[d->tex].set;
        }

        if (!cur_fb_w) {
            cur_fb_w = 1;
            vp.x = 0.0f;
            vp.y = 0.0f;
            vp.width = (float)(rt_w * rt_scale);
            vp.height = (float)(rt_h * rt_scale);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
        }

        {
            s32 x0 = d->scissor[0] < 0 ? 0 : d->scissor[0];
            s32 y0 = d->scissor[2] < 0 ? 0 : d->scissor[2];
            s32 x1 = d->scissor[1] + 1, y1 = d->scissor[3] + 1;
            if (x0 > (s32)rt_w - 1) x0 = (s32)rt_w - 1;
            if (y0 > (s32)rt_h - 1) y0 = (s32)rt_h - 1;
            if (x1 > (s32)rt_w) x1 = (s32)rt_w;
            if (y1 > (s32)rt_h) y1 = (s32)rt_h;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            sc.offset.x = x0 * (s32)rt_scale;
            sc.offset.y = y0 * (s32)rt_scale;
            sc.extent.width  = (u32)(x1 - x0) * rt_scale;
            sc.extent.height = (u32)(y1 - y0) * rt_scale;
        }
        if (!have_sc || memcmp(&sc, &bound_sc, sizeof sc)) {
            vkCmdSetScissor(cmd, 0, 1, &sc);
            bound_sc = sc; have_sc = 1;
        }

        bc[0] = bc[1] = bc[2] = bc[3] = fixa;
        if (bound_fix != d->fix) {
            vkCmdSetBlendConstants(cmd, bc);
            bound_fix = d->fix;
        }

        if (bound_pipe != pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdSetLineWidth(cmd, line_w);
            bound_pipe = pipe;
        }
        {   VkDescriptorSet dset = set;
            if (d->date && d->rt < RT_SLOTS && rts[d->rt].created) {
                VkDescriptorSet snap = target_snapshot(cmd, &rts[d->rt]);
                if (snap) dset = snap;
                cur_fb_w = 0;
                vb = 0;
            }
            {   VkDescriptorSet sets[2];
                sets[0] = set; sets[1] = dset;
                if (bound_sets[0] != sets[0] || bound_sets[1] != sets[1]) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            gs_layout, 0, 2, sets, 0, NULL);
                    bound_sets[0] = sets[0]; bound_sets[1] = sets[1];
                }
            }
        }

        memset(&pc, 0, sizeof(pc));
        pc.scale = (float)rt_scale;
        pc.point_size = point_sz;
        pc.inv_size[0] = 2.0f / (float)rt_w;
        pc.inv_size[1] = 2.0f / (float)rt_h;
        pc.tex_size[0] = d->tex_w > 0.0f ? d->tex_w : 1.0f;
        pc.tex_size[1] = d->tex_h > 0.0f ? d->tex_h : 1.0f;
        if (d->tex_rt) {
            pc.img_size[0] = (float)rt_w;
            pc.img_size[1] = (float)rt_h;
        } else if (d->tex != 0xFFFF && d->tex < ntextures) {
            pc.img_size[0] = (float)textures[d->tex].w;
            pc.img_size[1] = (float)textures[d->tex].h;
        } else {
            pc.img_size[0] = pc.img_size[1] = 1.0f;
        }
        pc.tex_scale = d->tex_rt ? (float)rt_scale : 1.0f;
        pc.flags[0] = d->frag_flags;
        pc.flags[1] = (d->tex != 0xFFFF || d->tex_rt) ? 1u : 0u;
        pc.flags[2] = d->wrap;
        pc.flags[3] = debug_solid;
        pc.clamp_uv[0] = d->minu;
        pc.clamp_uv[1] = d->maxu;
        pc.clamp_uv[2] = d->minv;
        pc.clamp_uv[3] = d->maxv;
        pc.fogcol[0] = (float)((d->fogcol >> 0) & 0xFF) / 255.0f;
        pc.fogcol[1] = (float)((d->fogcol >> 8) & 0xFF) / 255.0f;
        pc.fogcol[2] = (float)((d->fogcol >> 16) & 0xFF) / 255.0f;
        pc.fogcol[3] = 1.0f;
        pc.mesh_block = d->native == PS2_VK_NATIVE_MESH ? d->mblock : 0u;
        if (!have_pc || memcmp(&pc, &bound_pc, sizeof pc)) {
            vkCmdPushConstants(cmd, gs_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            bound_pc = pc; have_pc = 1;
        }
        if (trace_frame && stat_frames == trace_frame) {
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            float vr = 0.0f, vg = 0.0f, vb = 0.0f, va = 0.0f;
            u32 k;
            if (rep_verts && d->count && d->native != PS2_VK_NATIVE_MESH) {
                for (k = 0; k < d->count; k++) {
                    const ps2_vk_vertex *v = &rep_verts[d->first + k];
                    if (v->x < x0) x0 = v->x;
                    if (v->x > x1) x1 = v->x;
                    if (v->y < y0) y0 = v->y;
                    if (v->y > y1) y1 = v->y;
                }
                vr = rep_verts[d->first].r; vg = rep_verts[d->first].g;
                vb = rep_verts[d->first].b; va = rep_verts[d->first].a;
            }
            ps2_log("  draw %3u: seq=%u em=%u rt%u tex=%d n=%u x[%.1f..%.1f] "
                    "y[%.1f..%.1f] rgba(%.0f,%.0f,%.0f,%.0f) blend=%03X fix=%u "
                    "set=%s topo=%u flags=%08X wrap=%X texrt=%u zrt=%u depth=%03X", i, d->seq,
                    (unsigned)d->emitter, d->rt,
                    (int)(s16)d->tex, d->count,
                    x0, x1, y0, y1, vr, vg, vb, va, d->blend, d->fix,
                    set == white_set ? "white"
                        : (d->tex_rt && set == rts[d->tex_rt - 1].set ? "RT"
                                                                      : "tex"),
                    d->topology, d->frag_flags, d->wrap, d->tex_rt, zrt, dkey);
        }
        if (d->native == PS2_VK_NATIVE_MESH) {
            VkDeviceSize zero = 0;
            if (!mset_ok) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gs_layout,
                                        2, 1, &fr->mset, 0, NULL);
                mset_ok = 1;
            }
            if (vb != 2) {
                vkCmdBindVertexBuffers(cmd, 0, 1, &fr->mvbuf, &zero);
                vb = 2;
            }
            if (!ib_ok) {
                vkCmdBindIndexBuffer(cmd, fr->mibuf, 0, VK_INDEX_TYPE_UINT32);
                ib_ok = 1;
            }
            vkCmdDrawIndexed(cmd, d->count, 1, d->first, (int32_t)d->mvbase, 0);
            stat_mesh_draws++;
        } else {
            if (vb != 1) {
                VkDeviceSize zero = 0;
                if (rep_nverts) vkCmdBindVertexBuffers(cmd, 0, 1, &fr->vbuf, &zero);
                vb = 1;
            }
            vkCmdDraw(cmd, d->count, 1, d->first, 0);
        }
        stat_draws++;
    }

    if (clip_open) colclip_resolve(cmd, &rts[cur_rt], &rts[cur_zrt]);
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    if (ov_on > 0 && present_rt == 0u) ov_promote();
    stat_verts += rep_nverts;
}

static void present_frame(VkCommandBuffer cmd, u32 image_index) {
    VkRenderingAttachmentInfo col;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    present_push pp;
    float sw = (float)swap_extent.width, sh = (float)swap_extent.height;
    float aspect_dst = sw / (sh > 0.0f ? sh : 1.0f);
    float aspect_src = aspect_dst;
    {   static float override_a = -1.0f;
        if (override_a < 0.0f) {
            const char *e = getenv("PS2_ASPECT");
            override_a = e ? (float)atof(e) : 0.0f;
            if (override_a > 0.0f)
                ps2_log("present: display aspect forced to %.4f", override_a);
        }
        if (override_a > 0.0f) aspect_src = override_a;
        else switch (ps2_cfg.aspect) {
        case PS2_ASPECT_AUTO:
            aspect_src = ps2_cfg.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
            break;
        case PS2_ASPECT_4_3:    aspect_src = 4.0f / 3.0f; break;
        case PS2_ASPECT_16_9:   aspect_src = 16.0f / 9.0f; break;
        case PS2_ASPECT_CUSTOM: aspect_src = ps2_cfg.aspect_custom; break;
        default: break;
        }
    }
    float scale_x = 1.0f, scale_y = 1.0f;
    int letterboxed;
    ps2_vk_display_aspect = aspect_src;

    if (aspect_dst > aspect_src) scale_x = aspect_src / aspect_dst;
    else                          scale_y = aspect_dst / aspect_src;
    if (ps2_cfg.integer_scale && present_h > 0 && sh > 0.0f) {
        float k = floorf(sh * scale_y / (float)present_h);
        if (k >= 1.0f) {
            float shape = (sw * scale_x) / (sh * scale_y);
            float ih = k * (float)present_h;
            scale_y = ih / sh;
            scale_x = ih * shape / sw;
            if (scale_x > 1.0f) { scale_y /= scale_x; scale_x = 1.0f; }
        }
    }
    letterboxed = scale_x < 0.9999f || scale_y < 0.9999f;
    if (present_h > 0) {
        float k = ceilf(sh * scale_y / (float)present_h - 0.01f);
        auto_scale_target = k < 1.0f ? 1u
                          : k > (float)RT_SCALE_MAX ? RT_SCALE_MAX : (u32)k;
    }

    vk_target *shown = ov_show ? &ov_comp
                     : &rts[present_rt < RT_SLOTS && rts[present_rt].created
                            ? present_rt : 0];
    if (shown->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier(cmd, shown->color, VK_IMAGE_ASPECT_COLOR_BIT,
                shown->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
        shown->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    barrier(cmd, swap_images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    memset(&col, 0, sizeof(col));
    col.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    col.imageView = swap_views[image_index];
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = letterboxed || present_blank ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof(ri));
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = swap_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &ri);

    vp.x = 0.0f; vp.y = 0.0f; vp.width = sw; vp.height = sh;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    {
        float ox = (1.0f - scale_x) * 0.5f, oy = (1.0f - scale_y) * 0.5f;
        s32 ix = (s32)(ox * sw + 0.5f), iy = (s32)(oy * sh + 0.5f);
        s32 iw = (s32)(scale_x * sw + 0.5f), ih = (s32)(scale_y * sh + 0.5f);
        if (ix < 0) ix = 0;
        if (iy < 0) iy = 0;
        if (iw > (s32)swap_extent.width - ix) iw = (s32)swap_extent.width - ix;
        if (ih > (s32)swap_extent.height - iy) ih = (s32)swap_extent.height - iy;
        sc.offset.x = ix; sc.offset.y = iy;
        sc.extent.width = iw > 0 ? (u32)iw : 0u;
        sc.extent.height = ih > 0 ? (u32)ih : 0u;
        vkCmdSetScissor(cmd, 0, 1, &sc);

        memset(&pp, 0, sizeof(pp));
        pp.map[0] = ((float)present_w / (float)rt_w) / scale_x;
        pp.map[1] = ((float)present_h / (float)rt_h) / scale_y;
        pp.map[2] = (float)present_x / (float)rt_w - ox * pp.map[0];
        pp.map[3] = (float)present_y / (float)rt_h - oy * pp.map[1];
        pp.bound[0] = ox;
        pp.bound[1] = oy;
        pp.bound[2] = ox + scale_x;
        pp.bound[3] = oy + scale_y;
        pp.opts[0] = (float)ps2_cfg.scale_filter;
        pp.opts[1] = ps2_cfg.fxaa ? 1.0f : 0.0f;
        pp.opts[2] = ps2_cfg.sharpen;
        pp.opts[3] = ps2_cfg.saturation;
        pp.grade[0] = ps2_cfg.brightness;
        pp.grade[1] = ps2_cfg.contrast;
        pp.grade[2] = ps2_cfg.gamma;
        pp.grade[3] = (ps2_cfg.fxaa || ps2_cfg.sharpen > 0.001f
                       || ps2_cfg.brightness != 0.0f || ps2_cfg.contrast != 1.0f
                       || ps2_cfg.gamma != 1.0f || ps2_cfg.saturation != 1.0f)
                    ? 1.0f : 0.0f;
    }

    {
        static u64 last_key;
        u64 key = ((u64)present_w << 40) | ((u64)present_h << 24)
                | ((u64)(shown - rts) << 16) | swap_extent.width
                | ((u64)rt_scale << 56);
        if (key != last_key) {
            last_key = key;
            ps2_log("present: pipe=%p set=%p slot=%u created=%d layout=%d | "
                    "rt %ux%u x%u  disp %u,%u %ux%u  swap %ux%u | "
                    "scale %.3f,%.3f letterboxed=%d shaded %d,%d %ux%u | "
                    "map %.4f,%.4f,%.4f,%.4f  bound %.3f,%.3f,%.3f,%.3f",
                    (void *)present_pipe, (void *)shown->set,
                    (u32)(shown - rts), shown->created, (int)shown->layout,
                    rt_w, rt_h, rt_scale, present_x, present_y, present_w, present_h,
                    swap_extent.width, swap_extent.height,
                    scale_x, scale_y, letterboxed,
                    sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height,
                    pp.map[0], pp.map[1], pp.map[2], pp.map[3],
                    pp.bound[0], pp.bound[1], pp.bound[2], pp.bound[3]);
        }
    }
    if (!present_blank) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                present_layout, 0, 1, &shown->set, 0, NULL);
        vkCmdPushConstants(cmd, present_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pp), &pp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    ps2_ui_draw(cmd);
    vkCmdEndRendering(cmd);

    if (swap_buf && getenv("PS2_DUMP_SWAP")) {
        VkBufferImageCopy sr;
        barrier(cmd, swap_images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        memset(&sr, 0, sizeof(sr));
        sr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sr.imageSubresource.layerCount = 1;
        sr.imageExtent.width = swap_extent.width;
        sr.imageExtent.height = swap_extent.height;
        sr.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, swap_images[image_index],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               swap_buf, 1, &sr);
        swap_w = swap_extent.width;
        swap_h = swap_extent.height;
        barrier(cmd, swap_images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    } else
    barrier(cmd, swap_images[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    read_w = rt_w * rt_scale;
    read_h = rt_h * rt_scale;
    read_slot = shown == &ov_comp ? (present_rt < RT_SLOTS ? present_rt : 0u)
                                  : (u32)(shown - rts);
    ov_presented = shown == &ov_comp;
    if (depth_buf && rts[0].created
        && rts[0].dlayout != VK_IMAGE_LAYOUT_UNDEFINED
        && (VkDeviceSize)read_w * read_h * 4u <= depth_capacity) {
        VkImageLayout dl = rts[0].dlayout;
        VkBufferImageCopy r;
        barrier(cmd, rts[0].depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                dl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        memset(&r, 0, sizeof(r));
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = read_w;
        r.imageExtent.height = read_h;
        r.imageExtent.depth = 1;
        depth_w = read_w;
        depth_h = read_h;
        vkCmdCopyImageToBuffer(cmd, rts[0].depth,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               depth_buf, 1, &r);
        barrier(cmd, rts[0].depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    }
}

static void gpu_speed_collect(vk_frame *fr) {
    static u64 start, n;
    static double total, peak, scene, present;
    if (!fr->timing_pending) return;
    fr->timing_pending = 0;
    u64 stamps[3];
    VkResult result = vkGetQueryPoolResults(dev, fr->timing_queries, 0, 3,
        sizeof(stamps), stamps, sizeof(u64), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        ps2_log("gpu-speed: completed-frame query unavailable (%d)", (int)result);
        return;
    }
    u64 mask = gpu_timestamp_bits >= 64 ? ~(u64)0 : ((u64)1 << gpu_timestamp_bits) - 1;
    double ms = ((stamps[2] - stamps[0]) & mask) * gpu_timestamp_period / 1e6;
    total += ms;
    if (ms > peak) peak = ms;
    scene += ((stamps[1] - stamps[0]) & mask) * gpu_timestamp_period / 1e6;
    present += ((stamps[2] - stamps[1]) & mask) * gpu_timestamp_period / 1e6;
    n++;
    u64 now = vk_mono_ns();
    if (!start) start = now;
    if (now - start >= 5000000000ull) {
        ps2_log("gpu-speed: dt=%.2fs samples=%llu execution_ms avg=%.3f max=%.3f scene=%.3f upscale=%.3f (GPU timestamps; excludes queue backlog before first timestamp)",
            (now-start)/1e9, (unsigned long long)n, total/n, peak, scene/n, present/n);
        start = now; n = 0; total = peak = scene = present = 0;
    }
}

static int field_skip_present;

static void draw_and_present(void) {
    vk_frame *fr = &frames[frame_index];
    u32 image_index = 0;
    int show = !field_skip_present;
    VkResult r;
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPresentInfoKHR pi;
    int timing = renderer_speed_enabled(), submitted = 0;
    u64 times[VS_N] = {0}, begin = timing ? vk_mono_ns() : 0, mark = begin;

    r = vkWaitForFences(dev, 1, &fr->fence, VK_TRUE, VK_WAIT_NS);
    if (timing) { u64 now=vk_mono_ns(); times[VS_FENCE]=now-mark; mark=now; }
    if (r != VK_SUCCESS) goto finished;
    gpu_speed_collect(fr);
    if (show) {
        if (swap_dirty) {
            swap_dirty = 0;
            vkDeviceWaitIdle(dev);
            if (create_swapchain() != 0) goto finished;
        }
        r = vkAcquireNextImageKHR(dev, swapchain, VK_WAIT_NS, fr->acquired,
                                  VK_NULL_HANDLE, &image_index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(dev);
            if (create_swapchain() != 0) goto finished;
            r = vkAcquireNextImageKHR(dev, swapchain, VK_WAIT_NS, fr->acquired,
                                      VK_NULL_HANDLE, &image_index);
        }
        if (timing) { u64 now=vk_mono_ns(); times[VS_ACQUIRE]=now-mark; mark=now; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) goto finished;
    }
    vkResetFences(dev, 1, &fr->fence);

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(fr->cmd, 0);
    if (show && !present_blank) ov_prepare(); else ov_show = 0;
    vkBeginCommandBuffer(fr->cmd, &bi);
    if (fr->timing_queries) {
        vkCmdResetQueryPool(fr->cmd, fr->timing_queries, 0, 3);
        vkCmdWriteTimestamp(fr->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, fr->timing_queries, 0);
    }
    render_list(fr->cmd, fr);
    if (fr->timing_queries)
        vkCmdWriteTimestamp(fr->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fr->timing_queries, 1);
    if (show && ov_show) ov_compose(fr->cmd, fr);
    if (show) present_frame(fr->cmd, image_index);
    if (fr->timing_queries)
        vkCmdWriteTimestamp(fr->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fr->timing_queries, 2);
    vkEndCommandBuffer(fr->cmd);
    if (timing) { u64 now=vk_mono_ns(); times[VS_RECORD]=now-mark; mark=now; }

    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = show ? 1u : 0u;
    si.pWaitSemaphores = &fr->acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr->cmd;
    si.signalSemaphoreCount = show ? 1u : 0u;
    si.pSignalSemaphores = &fr->rendered;
    VkResult submit_result = vkQueueSubmit(queue, 1, &si, fr->fence);
    fr->timing_pending = fr->timing_queries && submit_result == VK_SUCCESS;
    if (timing) { u64 now=vk_mono_ns(); times[VS_SUBMIT]=now-mark; mark=now; }

    frame_index = (frame_index + 1) % FRAMES_IN_FLIGHT;
    submitted = 1;
    if (!show) goto finished;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &fr->rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &image_index;
    r = vkQueuePresentKHR(queue, &pi);
    if (timing) { u64 now=vk_mono_ns(); times[VS_PRESENT]=now-mark; }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_dirty = 1;
    stat_frames++;
finished:
    if (timing) {
        times[VS_TOTAL] = vk_mono_ns() - begin;
        renderer_speed_sample(times, submitted);
    }
}

enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT,
       B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };

static SDL_Gamepad *gamepad;

static int window_hidden;
void ps2_video_hidden(int on) { window_hidden = on; }

static int pad_deadzone_env(void) {
    static int cached = -2;
    if (cached == -2) {
        const char *e = getenv("PS2_PAD_DEADZONE");
        cached = e ? (int)strtol(e, NULL, 0) : -1;
        if (cached > 32000) cached = 32000;
    }
    return cached;
}

typedef struct { int rest; int have_rest; } pad_trigger;
static pad_trigger trig_l, trig_r;

static int trigger_raw(pad_trigger *t, int v) {
    if (!PS2_ENV("PS2_PAD_TRIGGER_REST")) return v;
    if (!t->have_rest) { t->rest = v; t->have_rest = 1; }
    v -= t->rest;
    return v < 0 ? -v : v;
}

static int padbind_is_trigger(int code) {
    int axis = (code & 0xFF) >> 1;
    return (code & PS2_PADBIND_AXIS)
        && (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
            || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

static float padbind_value(int code) {
    if (!gamepad || code <= 0) return 0.0f;
    if (code & PS2_PADBIND_AXIS) {
        int axis = (code & 0xFF) >> 1, v;
        if (axis >= SDL_GAMEPAD_AXIS_COUNT) return 0.0f;
        v = SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)axis);
        if (padbind_is_trigger(code))
            v = trigger_raw(axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? &trig_l : &trig_r, v);
        return ps2_pad_axis_value(v, code & 1);
    }
    if (code - 1 >= SDL_GAMEPAD_BUTTON_COUNT) return 0.0f;
    return SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)(code - 1)) ? 1.0f : 0.0f;
}

SDL_Gamepad *ps2_video_gamepad(void) { return gamepad; }

void ps2_video_select_gamepad(SDL_JoystickID id) {
    SDL_Gamepad *g;
    if (gamepad && SDL_GetGamepadID(gamepad) == id) return;
    g = SDL_OpenGamepad(id);
    if (!g) return;
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad = g;
    trig_l.have_rest = trig_r.have_rest = 0;
    ps2_log("pad: %s selected", SDL_GetGamepadName(gamepad));
}

static void poll_input(void) {
    const bool *keys = window_hidden ? NULL : SDL_GetKeyboardState(NULL);
    ps2_pad_state st;
    u16 kbmask = 0;
    float value[PS2_ACT_COUNT];
    float dz = ps2_cfg.deadzone;
    int env = pad_deadzone_env();
    memset(&st, 0, sizeof(st));
    st.connected = 1;
    st.lx = st.ly = st.rx = st.ry = PS2_PAD_ANALOG_NEUTRAL;
    if (ps2_ui_blocks_game_input()) { ps2_pad_publish(0, &st); return; }

    for (int act = 0; act < PS2_ACT_COUNT; act++) {
        float v = 0.0f;
        for (int k = 0; k < PS2_BIND_SLOTS; k++) {
            int sc = ps2_cfg.key[act][k], code = ps2_cfg.pad[act][k];
            if (keys && sc > 0 && sc < SDL_SCANCODE_COUNT && keys[sc]) {
                v = 1.0f;
                if (act < PS2_ACT_BUTTONS) kbmask |= (u16)(1u << act);
            }
            if (gamepad && code > 0) {
                float p = padbind_value(code);
                if (p > v) v = p;
            }
        }
        value[act] = v;
    }
    for (int act = 0; act < PS2_ACT_BUTTONS; act++) {
        int held;
        if (act == PS2_ACT_L2)
            held = ps2_pad_trigger(ps2_cfg.button_deadzone, value[act], &st.l2);
        else if (act == PS2_ACT_R2)
            held = ps2_pad_trigger(ps2_cfg.button_deadzone, value[act], &st.r2);
        else
            held = ps2_pad_button(ps2_cfg.button_deadzone, value[act]);
        if (held) st.buttons |= (u16)(1u << act);
    }
    if (env >= 0) dz = (float)env / 32767.0f;
    ps2_pad_stick(dz, ps2_cfg.axis_scale, ps2_cfg.invert[0], value + PS2_ACT_LS_UP,
                  &st.lx, &st.ly);
    ps2_pad_stick(dz, ps2_cfg.axis_scale, ps2_cfg.invert[1], value + PS2_ACT_RS_UP,
                  &st.rx, &st.ry);

    if (PS2_ENV("PS2_TRACE_PAD")) {
        static u16 last_src = 0xFFFF;
        if (st.buttons != last_src) {
            last_src = st.buttons;
            ps2_log("pad src: keyboard=%04X gamepad=%s final=%04X  "
                    "raw LT=%d RT=%d LX=%d LY=%d  out lx=%u ly=%u",
                    kbmask, gamepad ? "yes" : "no", st.buttons,
                    gamepad ? SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) : 0,
                    gamepad ? SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) : 0,
                    gamepad ? SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) : 0,
                    gamepad ? SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) : 0,
                    st.lx, st.ly);
        }
    }
    ps2_pad_publish(0, &st);
}

static int take_pending_frame(void) {
    int got;
    pthread_mutex_lock(&list_lock);
    got = frame_requested;
    if (got) {
        frame_requested = 0;
        pthread_cond_broadcast(&list_cv);
    }
    pthread_mutex_unlock(&list_lock);
    return got;
}

static void update_title(void) {
    static Uint64 last_ms;
    static u64 last_frames, last_dropped;
    Uint64 now = SDL_GetTicks();
    u64 fields = ps2_kernel_vblank_count();
    static u64 last_fields;
    char buf[192];
    double dt;
    if (window_hidden || !window) return;
    if (now - last_ms < 1000) return;
    dt = (double)(now - last_ms) / 1000.0;
    if (last_ms == 0) { last_ms = now; last_frames = stat_frames;
                        last_dropped = stat_dropped; last_fields = fields;
                        return; }
    snprintf(buf, sizeof(buf), "%s  -  %.0f fields/s, %.0f fps%s",
             window_title, (double)(fields - last_fields) / dt,
             (double)(stat_frames - last_frames) / dt,
             stat_dropped > last_dropped ? ", dropping" : "");
    SDL_SetWindowTitle(window, buf);
    last_ms = now;
    last_frames = stat_frames;
    last_dropped = stat_dropped;
    last_fields = fields;
}

#define FIELD_HZ 59.94
static u64 fps_cap_ns = 16000000ull;
static u64 fps_cap_last;
static double fps_limit_hz;
static double present_credit;

static u64 vk_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static int fps_cap_env;

static void fps_cap_set(double hz) {
    fps_limit_hz = hz > 0.0 && hz < FIELD_HZ ? hz : 0.0;
    fps_cap_ns = hz > 0.0 ? (u64)(1e9 / hz) : 16000000ull;
    present_credit = 0.0;
}

static void fps_cap_apply_settings(void) {
    if (fps_cap_env) return;
    fps_cap_set(ps2_cfg.fps_limit > 0 ? (double)ps2_cfg.fps_limit : 0.0);
}

static void fps_cap_init(void) {
    static int done;
    const char *e;
    if (done) return;
    done = 1;
    e = getenv("PS2_FPS_CAP");
    if (!(e && *e)) {
        fps_cap_apply_settings();
        return;
    }
    fps_cap_env = 1;
    fps_cap_set(atof(e));
    if (atof(e) <= 0.0) fps_cap_ns = 0;
    ps2_log("vk: presentation limited to %s fps%s", atof(e) > 0.0 ? e : "no",
            fps_limit_hz > 0.0 ? " -- fields are drawn but only some are shown" : "");
}

static int fps_cap_due(void) {
    u64 now;
    fps_cap_init();
    if (!fps_cap_ns) return 1;
    now = vk_mono_ns();
    if (fps_cap_last && now - fps_cap_last < fps_cap_ns) return 0;
    fps_cap_last = now;
    return 1;
}

static int field_present_due(void) {
    fps_cap_init();
    if (fps_limit_hz <= 0.0) return 1;
    present_credit += fps_limit_hz / FIELD_HZ;
    if (present_credit < 1.0) return 0;
    present_credit -= 1.0;
    if (present_credit > 1.0) present_credit = 1.0;
    return 1;
}

static u32 choose_scale(u32 need_w, u32 need_h) {
    static int env = -2;
    static u32 auto_last, asked_last;
    static Uint64 auto_since;
    u32 s, lim, big;
    int want;
    if (env == -2) {
        const char *e = getenv("PS2_UPSCALE");
        env = e && *e ? atoi(e) : -1;
        if (env >= 0)
            ps2_log("vk: PS2_UPSCALE=%d sets the internal resolution%s", env,
                    env == 0 ? " to Auto" : "");
    }
    want = env >= 0 ? env : ps2_cfg.internal_res;
    {
        static int cycle = -1;
        static const int steps[4] = { 1, 2, 4, 3 };
        if (cycle < 0) {
            const char *e = getenv("PS2_UPSCALE_CYCLE");
            cycle = e ? atoi(e) : 0;
        }
        if (cycle > 0) want = steps[(stat_frames / (u64)cycle) % 4u];
    }
    if (want <= 0) {
        if (auto_scale_target != auto_last) {
            auto_last = auto_scale_target;
            auto_since = SDL_GetTicks();
        }
        s = !rts[0].created || SDL_GetTicks() - auto_since >= 250
          ? auto_last : rt_scale;
    } else {
        s = (u32)want;
    }
    big = need_w > need_h ? need_w : need_h;
    if (big < rt_w) big = rt_w;
    if (big < rt_h) big = rt_h;
    lim = max_image_dim / (big ? big : 1u);
    if (lim > RT_SCALE_MAX) lim = RT_SCALE_MAX;
    if (s > lim) s = lim;
    if (s < 1u) s = 1u;
    if (s != asked_last) {
        asked_last = s;
        scale_failed = 0;
    }
    return s == scale_failed ? rt_scale : s;
}

static void replay_frame_inner(void);
static void replay_frame(void) {
    PS2_PHASE_BEGIN(PS2_PH_PRESENT);
    pthread_mutex_lock(&gpu_lock);
    replay_frame_inner();
    pthread_mutex_unlock(&gpu_lock);
    PS2_PHASE_END(PS2_PH_PRESENT);
    pthread_mutex_lock(&list_lock);
    frame_done = 1;
    pthread_cond_broadcast(&list_cv);
    pthread_mutex_unlock(&list_lock);
}
static void replay_frame_inner(void) {
    apply_sampler_settings();
    if (!window_minimised) {
        u32 need_w = present_x + present_w, need_h = present_y + present_h;
        u32 k;
        for (k = 0; k < rep_ndraws; k++) {
            u32 w = rep_draws[k].fb_w;
            u32 h = (u32)(rep_draws[k].scissor[3] + 1);
            if (w > need_w && w <= 1024u) need_w = w;
            if (h > need_h && h <= 1024u) need_h = h;
        }
        create_targets(need_w, need_h, choose_scale(need_w, need_h));
        draw_and_present();
    }
}

static bool SDLCALL modal_redraw_watch(void *ud, SDL_Event *e) {
    (void)ud;
    if (!vk_ready || in_modal_redraw || window_minimised) return true;
    if (SDL_GetCurrentThreadID() != render_tid) return true;
    switch (e->type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        swap_dirty = 1;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_MOVED:
        break;
    default:
        return true;
    }
    if (!fps_cap_due()) return true;
    in_modal_redraw = 1;
    take_pending_frame();
    replay_frame();
    in_modal_redraw = 0;
    return true;
}

static void test_render_stall(void) {
    static int ms = -1;
    static Uint64 next;
    Uint64 now;
    if (ms == -1) ms = (int)env_flag("PS2_TEST_RENDER_STALL");
    if (ms <= 0) return;
    now = SDL_GetTicks();
    if (now < next) return;
    next = now + 2000;
    ps2_log("vk: test stall, holding the renderer for %d ms", ms);
    SDL_Delay((Uint32)ms);
}

static void apply_window_settings(void) {
    if (window_hidden || !window) return;
    if (ps2_cfg.window_mode == PS2_WIN_WINDOWED) {
        want_fullscreen = 0;
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowSize(window, ps2_cfg.window_w, ps2_cfg.window_h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        SDL_DisplayMode mode;
        int ok = 0;
        if (ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE) {
            SDL_DisplayID d = SDL_GetDisplayForWindow(window);
            const SDL_DisplayMode *desk = d ? SDL_GetDesktopDisplayMode(d) : NULL;
            int w = ps2_cfg.fs_w > 0 ? ps2_cfg.fs_w : desk ? desk->w : 0;
            int h = ps2_cfg.fs_h > 0 ? ps2_cfg.fs_h : desk ? desk->h : 0;
            float hz = ps2_cfg.fs_hz > 0.0f ? ps2_cfg.fs_hz
                     : desk ? desk->refresh_rate : 0.0f;
            ok = w > 0 && h > 0
              && SDL_GetClosestFullscreenDisplayMode(d, w, h, hz, false, &mode);
        }
        SDL_SetWindowFullscreenMode(window, ok ? &mode : NULL);
        want_fullscreen = 1;
        SDL_SetWindowFullscreen(window, true);
    }
    swap_dirty = 1;
}

static void settings_service(void) {
    static Uint64 save_at;
    int d = ps2_settings_take_dirty();
    if (d & PS2_CFG_WINDOW) apply_window_settings();
    if (d & PS2_CFG_SWAPCHAIN) swap_dirty = 1;
    if (d & PS2_CFG_FPS) fps_cap_apply_settings();
    if (d & PS2_CFG_SAVE) save_at = SDL_GetTicks() + 750;
    if (save_at && SDL_GetTicks() >= save_at) {
        save_at = 0;
        ps2_settings_save();
    }
}

static void pump_events(void) {
    SDL_Event e;
    test_render_stall();
    while (SDL_PollEvent(&e)) {
        if (ps2_ui_event(&e)) continue;
        switch (e.type) {
        case SDL_EVENT_QUIT:
            if (!window_hidden) window_closed = 1;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (!window_hidden) window_closed = 1;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            swap_dirty = 1;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            swap_dirty = 1;
            if (ps2_cfg.window_mode == PS2_WIN_WINDOWED && !want_fullscreen
                && !window_hidden && e.window.data1 > 0 && e.window.data2 > 0
                && (e.window.data1 != ps2_cfg.window_w
                    || e.window.data2 != ps2_cfg.window_h)) {
                ps2_cfg.window_w = e.window.data1;
                ps2_cfg.window_h = e.window.data2;
                ps2_settings_touch(PS2_CFG_SAVE);
            }
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            window_minimised = 1;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_EXPOSED:
            window_minimised = 0;
            swap_dirty = 1;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad) {
                gamepad = SDL_OpenGamepad(e.gdevice.which);
                trig_l.have_rest = trig_r.have_rest = 0;
                if (gamepad) ps2_log("pad: %s connected",
                                     SDL_GetGamepadName(gamepad));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad) { SDL_CloseGamepad(gamepad); gamepad = NULL; }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE && !window_hidden) window_closed = 1;
            if (e.key.key == SDLK_F9) {
                ps2_capture_request = 1;
                ps2_log("capture: requested; the next field will be captured");
                printf("[F9] capturing this screen...\n");
                fflush(stdout);
            }
            if (e.key.key == SDLK_F6) {
                int was = ps2_cap_active();
                ps2_cap_toggle();
                printf(was ? "[F6] capture stopping at the next field...\n"
                           : "[F6] capture armed; press F6 again to stop it\n");
                fflush(stdout);
            }
            if (e.key.key == SDLK_F7) {
                trace_frame = stat_frames + 1u;
                ps2_log("draw trace: frame %llu armed from F7",
                        (unsigned long long)trace_frame);
                printf("[F7] tracing next frame's draws...\n");
                fflush(stdout);
            }
            if (e.key.key == SDLK_F8) {
                ps2_statecap_request("hotkey");
                ps2_log("state: capture requested from F8");
                printf("[F8] capturing machine state...\n");
                fflush(stdout);
            }
            if (e.key.key == SDLK_F10) {
                ps2_diag_armed = !ps2_diag_armed;
                ps2_log("capture: censuses %s",
                        ps2_diag_armed ? "ON" : "off");
                printf("[F10] censuses %s\n", ps2_diag_armed ? "ON" : "off");
                fflush(stdout);
            }
            if (e.key.key == SDLK_F11
                || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                ps2_cfg.window_mode = want_fullscreen ? PS2_WIN_WINDOWED
                                                      : ps2_cfg.fullscreen_type;
                apply_window_settings();
                ps2_settings_touch(PS2_CFG_SAVE);
            }
            break;
        default:
            break;
        }
    }
    settings_service();
    poll_input();
}

static void *renderer_main(void *arg) {
    (void)arg;
    render_tid = SDL_GetCurrentThreadID();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        ps2_log("vk: SDL_Init: %s", SDL_GetError());
        pthread_mutex_lock(&list_lock);
        renderer_running = -1;
        pthread_cond_broadcast(&list_cv);
        pthread_mutex_unlock(&list_lock);
        return NULL;
    }
    window = SDL_CreateWindow((const char *)arg ? (const char *)arg
                                                : "ps2recomp",
                              ps2_cfg.window_w, ps2_cfg.window_h,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
                              | (window_hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        ps2_log("vk: SDL_CreateWindow: %s", SDL_GetError());
        pthread_mutex_lock(&list_lock);
        renderer_running = -1;
        pthread_cond_broadcast(&list_cv);
        pthread_mutex_unlock(&list_lock);
        return NULL;
    }
    if (vk_init() != 0) {
        ps2_log("vk: initialisation failed; falling back to the software "
                "rasteriser");
        pthread_mutex_lock(&list_lock);
        renderer_running = -1;
        pthread_cond_broadcast(&list_cv);
        pthread_mutex_unlock(&list_lock);
        return NULL;
    }
    vk_ready = 1;
    SDL_AddEventWatch(modal_redraw_watch, NULL);
    {   ps2_ui_init_info ui;
        memset(&ui, 0, sizeof(ui));
        ui.window = window;
        ui.instance = inst;
        ui.phys = phys;
        ui.device = dev;
        ui.queue_family = qfamily;
        ui.queue = queue;
        ui.color_format = swap_format;
        ui.image_count = swap_count;
        ui.max_anisotropy = max_aniso;
        if (ps2_ui_init(&ui) != 0) ps2_log("ui: settings menu unavailable");
    }
    if (ps2_cfg.window_mode != PS2_WIN_WINDOWED) apply_window_settings();
    {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids) {
            if (n > 0 && !gamepad) {
                gamepad = SDL_OpenGamepad(ids[0]);
                if (gamepad)
                    ps2_log("pad: %s connected", SDL_GetGamepadName(gamepad));
            }
            SDL_free(ids);
        }
    }
    pthread_mutex_lock(&list_lock);
    renderer_running = 1;
    pthread_cond_broadcast(&list_cv);
    pthread_mutex_unlock(&list_lock);

    for (;;) {
        struct timespec ts;
        pump_events();
        if (pipe_warm_n) pipe_warm_step(4000000ull);
        else if (npipes > pipe_saved_n && vk_mono_ns() - pipe_saved_ns > 20000000000ull)
            pipe_cache_save(0);
        pthread_mutex_lock(&list_lock);
        if (renderer_running <= 0) { pthread_mutex_unlock(&list_lock); break; }
        if (!frame_requested) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 4 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&list_cv, &list_lock, &ts);
            if (!frame_requested) { pthread_mutex_unlock(&list_lock); continue; }
        }
        rep_verts  = pub_verts;
        rep_draws  = pub_draws;
        rep_nverts = pub_nverts;
        rep_ndraws = pub_ndraws;
        rep_mesh   = pub_mesh;
        frame_requested = 0;
        pthread_cond_broadcast(&list_cv);
        pthread_mutex_unlock(&list_lock);
        field_skip_present = !field_present_due();
        replay_frame();
        if (!field_skip_present) fps_cap_last = vk_mono_ns();
        field_skip_present = 0;
        update_title();
    }
    SDL_RemoveEventWatch(modal_redraw_watch, NULL);
    if (ps2_settings_save_pending()) ps2_settings_save();
    pthread_mutex_lock(&gpu_lock);
    if (dev) vkDeviceWaitIdle(dev);
    pipe_cache_save(1);
    ps2_ui_shutdown();
    pthread_mutex_unlock(&gpu_lock);
    return NULL;
}

static pthread_t renderer_thread;

int ps2_video_init(const char *title, int width, int height) {
    { const char *o = getenv("PS2_DEBUG_SOLID");
      debug_solid = o ? (u32)strtoul(o, NULL, 0) : 0u;
      o = getenv("PS2_TRACE_FRAME");
      trace_frame = o ? strtoull(o, NULL, 0) : 0u; }
    snprintf(window_title, sizeof(window_title), "%s", title ? title : "ps2recomp");
    present_w = (u32)(width > 0 ? width : 640);
    present_h = (u32)(height > 0 ? height : 448);

    {   int s;
        for (s = 0; s < LIST_SLOTS; s++) {
            slot_verts[s] = (ps2_vk_vertex *)malloc(sizeof(ps2_vk_vertex)
                                                    * VK_MAX_VERTS);
            slot_draws[s] = (vk_draw *)malloc(sizeof(vk_draw) * VK_MAX_DRAWS);
            if (!slot_verts[s] || !slot_draws[s]) return -1;
            slot_mesh[s].recs = (u8 *)malloc((size_t)MESH_MAX_VERTS * MESH_REC_BYTES);
            slot_mesh[s].idx = (u32 *)malloc((size_t)MESH_MAX_INDICES * sizeof(u32));
            slot_mesh[s].blocks = (ps2_vk_mesh_block *)malloc(
                (size_t)MESH_MAX_BLOCKS * sizeof(ps2_vk_mesh_block));
        }
    }
    rec_slot  = 0;
    pub_slot  = LIST_SLOTS - 1;
    rec_verts = slot_verts[rec_slot];
    rec_draws = slot_draws[rec_slot];
    rep_verts = slot_verts[LIST_SLOTS - 1];
    rep_draws = slot_draws[LIST_SLOTS - 1];
    pub_verts = rep_verts;
    pub_draws = rep_draws;
    rec_mesh  = &slot_mesh[rec_slot];
    rep_mesh  = &slot_mesh[LIST_SLOTS - 1];
    pub_mesh  = rep_mesh;

    {
        const char *e = getenv("PS2_LOCKSTEP");
        if (e && *e && *e != '0') {
            ps2_vk_lockstep = 1;
            ps2_log("vk: lockstep present -- fields are waited for, never dropped");
        }
    }
    if (pthread_create(&renderer_thread, NULL, renderer_main, window_title) != 0)
        return -1;
    pthread_mutex_lock(&list_lock);
    while (renderer_running == 0)
        pthread_cond_wait(&list_cv, &list_lock);
    pthread_mutex_unlock(&list_lock);
    return renderer_running > 0 ? 0 : -1;
}

int ps2_vk_enabled(void) {
    return vk_ready && !window_closed && renderer_running > 0;
}

void ps2_video_render_info(ps2_render_info *out) {
    u32 n = 0, snaps = 0, big = rt_w > rt_h ? rt_w : rt_h, i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < RT_SLOTS; i++) {
        if (!rts[i].created) continue;
        n++;
        if (rts[i].snap) snaps++;
    }
    out->scale = rt_scale;
    out->guest_w = rt_w;
    out->guest_h = rt_h;
    out->targets = n;
    out->bytes_at_1x = (uint64_t)rt_w * rt_h * (8u * n + 4u * snaps);
    out->max_scale = max_image_dim / (big ? big : 1u);
    if (out->max_scale > RT_SCALE_MAX) out->max_scale = RT_SCALE_MAX;
    if (out->max_scale < 1u) out->max_scale = 1u;
    out->auto_scale = auto_scale_target;
    out->failed_scale = scale_failed;
    out->max_line_width = max_line_width;
}

int ps2_vk_closed(void) {
    return vk_ready && (window_closed || renderer_running <= 0);
}

static void ps2_vk_draw_inner(int topology, const ps2_vk_state *st,
                              const ps2_vk_vertex *v, int n);
static u16 rec_native_mark;
void ps2_vk_draw(int topology, const ps2_vk_state *st,
                 const ps2_vk_vertex *v, int n) {
    if (ps2_vk_before_record) ps2_vk_before_record();
    PS2_PHASE_BEGIN(PS2_PH_VKDRAW);
    ps2_vk_draw_inner(topology, st, v, n);
    PS2_PHASE_END(PS2_PH_VKDRAW);
}
void ps2_vk_draw_2d(int topology, const ps2_vk_state *st,
                    const ps2_vk_vertex *v, int n) {
    if (ps2_vk_before_record) ps2_vk_before_record();
    PS2_PHASE_BEGIN(PS2_PH_VKDRAW);
    rec_native_mark = PS2_VK_NATIVE_2D;
    ps2_vk_draw_inner(topology, st, v, n);
    rec_native_mark = 0;
    PS2_PHASE_END(PS2_PH_VKDRAW);
}

static struct { u32 vbase, ifirst, icount, block, cull; } rec_mesh_pending;

int ps2_vk_mesh_material_ok(const ps2_vk_state *st) {
    static int sw = -1;
    if (sw < 0) {
        const char *e = getenv("PS2_RN_MESH_MAT");
        sw = !(e && *e == '0');
    }
    if (!sw || !mesh_fs) return 0;
    if (!st) return 1;
    return !st->date && !st->colclip && !st->shuffle_rg && !st->tex_self
        && !st->tex_depth && !st->zcopy && !st->fst && st->wms < 3u && st->wmt < 3u;
}

int ps2_vk_mesh_available(void) {
    return vk_ready && mesh_ok && rec_mesh && rec_mesh->recs && rec_mesh->idx
        && rec_mesh->blocks;
}

int ps2_vk_mesh_store(const u8 *records, u32 n_verts, const u32 *indices,
                      u32 n_indices, u32 *vbase, u32 *ifirst) {
    mesh_arena *a = rec_mesh;
    if (!ps2_vk_mesh_available()) return -1;
    if (a->nrecs + n_verts > MESH_MAX_VERTS || a->nidx + n_indices > MESH_MAX_INDICES) {
        stat_mesh_overflow++;
        return -1;
    }
    memcpy(a->recs + (size_t)a->nrecs * MESH_REC_BYTES, records,
           (size_t)n_verts * MESH_REC_BYTES);
    memcpy(a->idx + a->nidx, indices, (size_t)n_indices * sizeof(u32));
    *vbase = a->nrecs;
    *ifirst = a->nidx;
    a->nrecs += n_verts;
    a->nidx += n_indices;
    return 0;
}

int ps2_vk_mesh_room(u32 n_verts, u32 n_indices, u32 n_draws) {
    const mesh_arena *a = rec_mesh;
    if (!ps2_vk_mesh_available()) return 0;
    return a->nrecs + n_verts <= MESH_MAX_VERTS && a->nidx + n_indices <= MESH_MAX_INDICES
        && a->nblocks + n_draws <= MESH_MAX_BLOCKS;
}

void ps2_vk_draw_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                      u32 vbase, u32 ifirst, u32 n_indices, int cull) {
    ps2_vk_draw_mesh_topo(st, b, vbase, ifirst, n_indices, cull, PS2_VK_TRIANGLES);
}

void ps2_vk_draw_mesh_topo(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                           u32 vbase, u32 ifirst, u32 n_indices, int cull,
                           int topology) {
    mesh_arena *a = rec_mesh;
    if (!ps2_vk_mesh_available() || !n_indices) return;
    if (ps2_vk_before_record) ps2_vk_before_record();
    if (a->nblocks >= MESH_MAX_BLOCKS) { stat_mesh_overflow++; return; }
    a->blocks[a->nblocks] = *b;
    rec_mesh_pending.vbase = vbase;
    rec_mesh_pending.ifirst = ifirst;
    rec_mesh_pending.icount = n_indices;
    rec_mesh_pending.block = a->nblocks++;
    rec_mesh_pending.cull = (cull & PS2_VK_MESH_CULL) ? 1u : 0u;
    {   int sw = ps2_vk_mesh_material_ok(NULL);
        if (cull & PS2_VK_MESH_GSFRAG) {
        } else if (ps2_vk_mesh_material_ok(st)) {
            rec_mesh_pending.cull |= 2u;
            stat_mesh_mat++;
        } else if (sw) {
            static int shown;
            if (shown++ < 8)
                ps2_log("vk: mesh keeps gs.frag: mesh_fs=%d date=%d colclip=%d shuffle=%d "
                        "tex_rt=%u depth=%d zcopy=%d fst=%d wms=%u wmt=%u", mesh_fs != 0,
                        st->date, st->colclip, st->shuffle_rg, st->tex_rt, st->tex_depth,
                        st->zcopy, st->fst, st->wms, st->wmt);
        }
    }
    PS2_PHASE_BEGIN(PS2_PH_VKDRAW);
    if (topology != PS2_VK_TRIANGLES) rec_mesh_pending.cull &= ~1u;
    rec_native_mark = PS2_VK_NATIVE_MESH;
    ps2_vk_draw_inner(topology, st, NULL, 0);
    rec_native_mark = 0;
    PS2_PHASE_END(PS2_PH_VKDRAW);
}
static void afail_mask(u32 afail, int *rgb, int *a, int *z) {
    if (afail == 1u) { *z = 0; }
    else if (afail == 2u) { *rgb = 0; *a = 0; }
    else { *a = 0; *z = 0; }
}

static int tex_point_for(const ps2_vk_state *st) {
    if (st->tex_rt || st->fst) return st->tex_point;
    if (ps2_cfg.tex_filter == PS2_TEXFILTER_NEAREST) return 1;
    if (ps2_cfg.tex_filter == PS2_TEXFILTER_BILINEAR) return 0;
    return st->tex_point;
}

static void rec_one(int topology, const ps2_vk_state *st,
                    const ps2_vk_vertex *v, int n, u32 flags, u32 depth_key);

void (*ps2_vk_before_record)(void);

static void ps2_vk_draw_inner(int topology, const ps2_vk_state *st,
                              const ps2_vk_vertex *v, int n) {
    u32 flags, depth_key;
    int discard_on_fail, write_rgb, write_a, write_z;
    static int no_split = -1;
    int split;
    if (no_split < 0) {
        const char *e = getenv("PS2_NO_AFAIL_SPLIT");
        no_split = (e && *e && *e != '0') ? 1 : 0;
    }
    split = !no_split && st->ate && st->atst != 0u && st->atst != 1u
            && st->afail != 0u;
    int passes = split ? 2 : 1;
    if (!vk_ready) return;
    if (rec_nverts + (u32)(n * passes) > VK_MAX_VERTS
        || rec_ndraws + (u32)passes > VK_MAX_DRAWS) {
        rec_overflow = 1;
        return;
    }
    {
        int test_can_fail = st->ate && st->atst != 1;
        u32 afail = st->ate ? st->afail : 0u;
        discard_on_fail = 1;
        write_rgb = 1;
        write_a = 1;
        write_z = st->zwrite ? 1 : 0;
        if (test_can_fail && afail != 0u && !split) {
            discard_on_fail = 0;
            afail_mask(afail, &write_rgb, &write_a, &write_z);
        }
    }
    flags = (st->tfx & 3u)
          | ((u32)(st->fst ? 1 : 0) << 2)
          | ((u32)(st->ate ? 1 : 0) << 3)
          | ((st->atst & 7u) << 4)
          | ((st->aref & 0xFFu) << 8)
          | ((u32)(st->fge && !vk_env_flag("PS2_NO_FOG", &vk_no_fog) ? 1 : 0) << 16)
          | ((u32)(st->tcc ? 1 : 0) << 17)
          | ((u32)discard_on_fail << 18)
          | ((u32)(tex_point_for(st) ? 1 : 0) << 19)
          | ((u32)(st->fba ? 1 : 0) << 21)
          | ((u32)(st->date ? 1 : 0) << 22)
          | ((u32)(st->datm ? 1 : 0) << 23);
    flags |= (u32)(st->shuffle_rg ? 1 : 0) << 24;
    {   static int old_centre = -1;
        int half = st->tex_rt ? !st->tex_depth
                 : (st->tme && !vk_env_flag("PS2_OLD_TEXEL_CENTRE", &old_centre));
        flags |= (u32)(half ? 1 : 0) << 25;
    }
    if (st->shuffle_rg) flags |= (st->shuffle_alpha & 7u) << 26;
    if (tex_point_for(st) == 2) flags |= 1u << 29;
    {   static int lock = -1;
        if (lock < 0) {
            const char *e = getenv("PS2_LOCK_ALPHA");
            lock = (e && *e && *e != '0') ? 1 : 0;
        }
        if (lock) write_a = 0;
    }
    static int trace_mask_env = -1;
    if (vk_env_flag("PS2_TRACE_MASK", &trace_mask_env)) {
        static int nlog;
        if (nlog < 40) {
            nlog++;
            ps2_log("mask: ate=%d atst=%u aref=%u afail=%u -> rgb=%d a=%d z=%d "
                    "discard=%d | abe=%d A=%u B=%u C=%u D=%u | date=%d datm=%d",
                    st->ate, st->atst, st->aref, st->afail,
                    write_rgb, write_a, write_z, discard_on_fail, st->abe,
                    st->alpha_a, st->alpha_b, st->alpha_c, st->alpha_d,
                    st->date, st->datm);
        }
    }
    depth_key = ((st->zte ? st->ztst : 1u) & 3u)
              | ((u32)write_z << 2)
              | ((u32)write_rgb << 3)
              | ((u32)write_a << 4);
    u32 extra_key = (st->colclip && colclip_ok ? 1u << 9 : 0u)
                  | (st->zcopy && zcopy_ok ? 1u << 10 : 0u);
    depth_key |= extra_key;

    rec_one(topology, st, v, n, flags, depth_key);
    if (split) {
        write_rgb = 1; write_a = 1; write_z = st->zwrite ? 1 : 0;
        afail_mask(st->afail, &write_rgb, &write_a, &write_z);
        rec_one(topology, st, v, n,
                (flags & ~(1u << 18)) | (1u << 20),
                ((st->zte ? st->ztst : 1u) & 3u) | ((u32)write_z << 2)
                    | ((u32)write_rgb << 3) | ((u32)write_a << 4) | extra_key);
    }
}

static void rec_one(int topology, const ps2_vk_state *st,
                    const ps2_vk_vertex *v, int n, u32 flags, u32 depth_key) {
    vk_draw *d;
    if (st->tme && !st->tex_rt && st->tex_index < ntextures)
        textures[st->tex_index].rec_seq = tex_rec_seq;
    for (u32 ch = 0; ch < 4; ch++)
        if (((st->fb_mask >> (ch * 8u)) & 255u) == 255u)
            depth_key |= 1u << (5u + ch);
    if (st->fb_psm == 1u) depth_key |= 1u << 8;
    if (rec_ndraws && rec_native_mark != PS2_VK_NATIVE_MESH) {
        d = &rec_draws[rec_ndraws - 1];
        if (d->native == rec_native_mark && d->topology == (u16)topology
            && d->tex == (u16)(st->tme && !st->tex_rt ? st->tex_index : 0xFFFFu)
            && d->frag_flags == flags
            && d->blend == (((u32)st->abe << 8) | st->alpha_a
                            | (st->alpha_b << 2) | (st->alpha_c << 4)
                            | (st->alpha_d << 6))
            && d->fix == st->alpha_fix
            && d->depth == depth_key
            && d->fb_w == st->fb_w && d->fb_h == st->fb_h
            && d->rt == (u16)st->rt && d->tex_rt == (u16)st->tex_rt
            && d->zrt == (u16)st->zrt
            && d->emitter == (u16)st->emitter
            && d->tex_self == (u16)st->tex_self
            && d->tex_depth == (u16)st->tex_depth
            && d->tex_w == st->tex_w && d->tex_h == st->tex_h
            && d->date == (u16)st->date
            && d->fogcol == st->fogcol
            && d->wrap == (st->wms | (st->wmt << 2))
            && d->minu == (u16)st->minu && d->maxu == (u16)st->maxu
            && d->minv == (u16)st->minv && d->maxv == (u16)st->maxv
            && d->scissor[0] == st->scissor[0] && d->scissor[1] == st->scissor[1]
            && d->scissor[2] == st->scissor[2] && d->scissor[3] == st->scissor[3]
            && d->first + d->count == rec_nverts) {
            memcpy(&rec_verts[rec_nverts], v, sizeof(*v) * (size_t)n);
            rec_nverts += (u32)n;
            d->count += (u32)n;
            return;
        }
    }
    if (ps2_verbose && stat_frames > 200 && stat_frames < 203 && rec_ndraws < 14)
        ps2_log("vk: draw topo=%d fb=%ux%u scis=%d..%d,%d..%d z=%u/%d abe=%d "
                "tex=%d(%gx%g) tfx=%u fst=%d ate=%d/%u/%u v0=(%.1f,%.1f,%.3f) "
                "c0=(%.0f,%.0f,%.0f,%.0f)",
                topology, st->fb_w, st->fb_h,
                st->scissor[0], st->scissor[1], st->scissor[2], st->scissor[3],
                st->ztst, st->zwrite, st->abe,
                (int)(s32)st->tex_index, st->tex_w, st->tex_h, st->tfx, st->fst,
                st->ate, st->atst, st->aref,
                v[0].x, v[0].y, v[0].z, v[0].r, v[0].g, v[0].b, v[0].a);
    d = &rec_draws[rec_ndraws++];
    d->topology = (u16)topology;
    d->tex = (u16)(st->tme && !st->tex_rt ? st->tex_index : 0xFFFFu);
    d->first = rec_nverts;
    d->count = (u32)n;
    d->frag_flags = flags;
    d->blend = ((u32)st->abe << 8) | st->alpha_a | (st->alpha_b << 2)
             | (st->alpha_c << 4) | (st->alpha_d << 6);
    d->fix = st->alpha_fix;
    d->depth = depth_key;
    d->seq = (u32)ps2_gs_prim_seq;
    d->tex_w = st->tex_w;
    d->tex_h = st->tex_h;
    d->fogcol = st->fogcol;
    d->scissor[0] = st->scissor[0];
    d->scissor[1] = st->scissor[1];
    d->scissor[2] = st->scissor[2];
    d->scissor[3] = st->scissor[3];
    d->fb_w = st->fb_w;
    d->fb_h = st->fb_h;
    d->rt = (u16)st->rt;
    d->zrt = (u16)st->zrt;
    d->emitter = (u16)st->emitter;
    d->native = rec_native_mark;
    d->tex_rt = (u16)st->tex_rt;
    d->tex_self = (u16)st->tex_self;
    d->tex_depth = (u16)st->tex_depth;
    d->date = (u16)st->date;
    d->wrap = st->wms | (st->wmt << 2) | (st->native_region ? 0x100u : 0u);
    d->minu = (u16)st->minu;
    d->maxu = (u16)st->maxu;
    d->minv = (u16)st->minv;
    d->maxv = (u16)st->maxv;
    if (rec_native_mark == PS2_VK_NATIVE_MESH) {
        d->first = rec_mesh_pending.ifirst;
        d->count = rec_mesh_pending.icount;
        d->mvbase = rec_mesh_pending.vbase;
        d->mblock = rec_mesh_pending.block;
        d->mcull = (u16)rec_mesh_pending.cull;
        return;
    }
    d->mvbase = 0;
    d->mblock = 0;
    d->mcull = 0;
    memcpy(&rec_verts[rec_nverts], v, sizeof(*v) * (size_t)n);
    rec_nverts += (u32)n;
}

void ps2_vk_native_draw(const ps2_vk_native *nd, const ps2_vk_vertex *v, int n) {
    vk_draw *d;
    if (!vk_ready || n <= 0) return;
    if (ps2_vk_before_record) ps2_vk_before_record();
    if (rec_nverts + (u32)n > VK_MAX_VERTS || rec_ndraws + 1u > VK_MAX_DRAWS) {
        rec_overflow = 1;
        return;
    }
    if (rec_ndraws) {
        d = &rec_draws[rec_ndraws - 1];
        if (d->native == (u16)nd->pass && d->tex == (u16)nd->tex
            && d->rt == (u16)nd->rt && d->zrt == (u16)nd->zrt
            && d->npoint == (u16)(nd->tex_point ? 1 : 0)
            && d->wrap == nd->wrap
            && d->tex_w == nd->tex_w && d->tex_h == nd->tex_h
            && !memcmp(d->nbox, nd->box, sizeof d->nbox)
            && d->nsun_z == nd->sun_z
            && d->scissor[0] == nd->scissor[0] && d->scissor[1] == nd->scissor[1]
            && d->scissor[2] == nd->scissor[2] && d->scissor[3] == nd->scissor[3]
            && d->first + d->count == rec_nverts) {
            memcpy(&rec_verts[rec_nverts], v, sizeof(*v) * (size_t)n);
            rec_nverts += (u32)n;
            d->count += (u32)n;
            return;
        }
    }
    d = &rec_draws[rec_ndraws++];
    memset(d, 0, sizeof *d);
    d->seq = (u32)ps2_gs_prim_seq;
    d->topology = PS2_VK_TRIANGLES;
    d->native = (u16)nd->pass;
    d->tex = (u16)(nd->tex < 0xFFFFu ? nd->tex : 0xFFFFu);
    d->first = rec_nverts;
    d->count = (u32)n;
    d->rt = (u16)nd->rt;
    d->zrt = (u16)nd->zrt;
    d->tex_w = nd->tex_w;
    d->tex_h = nd->tex_h;
    d->wrap = nd->wrap;
    d->npoint = (u16)(nd->tex_point ? 1 : 0);
    memcpy(d->nbox, nd->box, sizeof d->nbox);
    d->nsun_z = nd->sun_z;
    for (int i = 0; i < 4; i++) d->scissor[i] = nd->scissor[i];
    d->fb_w = rt_w;
    d->fb_h = rt_h;
    memcpy(&rec_verts[rec_nverts], v, sizeof(*v) * (size_t)n);
    rec_nverts += (u32)n;
}

u32 ps2_vk_texture_native(u32 src, u32 x, u32 y, u32 w, u32 h) {
    static u8 *buf;
    static size_t cap;
    vk_texture *s;
    const u8 *px;
    u64 key;
    u32 hash, idx, levels = 1;
    if (!vk_ready || src >= ntextures) return 0xFFFFFFFFu;
    s = &textures[src];
    px = __atomic_load_n(&s->rgba, __ATOMIC_ACQUIRE);
    if (!px || s->levels > 1u || x >= s->w || y >= s->h) return 0xFFFFFFFFu;
    if (w > s->w - x) w = s->w - x;
    if (h > s->h - y) h = s->h - y;
    if (!w || !h) return 0xFFFFFFFFu;
    key = (1ull << 63) | ((s->key * 0x9E3779B97F4A7C15ull) >> 1);
    key ^= ((u64)x << 42) ^ ((u64)y << 30) ^ ((u64)w << 16) ^ (u64)h;
    key |= 1ull << 63;
    hash = (s->hash * 16777619u) ^ (x * 73856093u) ^ (y * 19349663u)
         ^ (w * 83492791u) ^ h;
    for (s32 i = tex_first(key); i >= 0; i = tex_next[i])
        if (textures[i].key == key && textures[i].hash == hash
            && textures[i].w == w && textures[i].h == h) {
            textures[i].rec_seq = tex_rec_seq;
            return (u32)i;
        }
    if ((size_t)w * h * 4u > cap) {
        u8 *n = (u8 *)realloc(buf, (size_t)w * h * 4u);
        if (!n) return 0xFFFFFFFFu;
        buf = n;
        cap = (size_t)w * h * 4u;
    }
    for (u32 r = 0; r < h; r++)
        memcpy(buf + (size_t)r * w * 4u, px + ((size_t)(y + r) * s->w + x) * 4u,
               (size_t)w * 4u);
    idx = ps2_vk_texture(key, hash, buf, w, h);
    if (idx == 0xFFFFFFFFu) return idx;
    while ((w >> levels) || (h >> levels)) levels++;
    if (!textures[idx].image) textures[idx].levels = levels;
    return textures[idx].levels > 1u ? idx : 0xFFFFFFFFu;
}

u32 ps2_vk_texture_native_find(u64 key, u32 hash, u32 w, u32 h) {
    if (!vk_ready || !w || !h) return 0xFFFFFFFFu;
    key |= 1ull << 63;
    for (s32 i = tex_first(key); i >= 0; i = tex_next[i])
        if (textures[i].key == key && textures[i].hash == hash
            && textures[i].w == w && textures[i].h == h) {
            textures[i].rec_seq = tex_rec_seq;
            return (u32)i;
        }
    return 0xFFFFFFFFu;
}

u32 ps2_vk_texture_native_rgba(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h) {
    u32 idx, levels = 1;
    if (!vk_ready || !w || !h) return 0xFFFFFFFFu;
    idx = ps2_vk_texture_native_find(key, hash, w, h);
    if (idx != 0xFFFFFFFFu) return idx;
    key |= 1ull << 63;
    idx = ps2_vk_texture(key, hash, rgba, w, h);
    if (idx == 0xFFFFFFFFu) return idx;
    while ((w >> levels) || (h >> levels)) levels++;
    if (!textures[idx].image) textures[idx].levels = levels;
    return textures[idx].levels > 1u ? idx : 0xFFFFFFFFu;
}

int ps2_vk_texture_is(u32 idx, u64 key, u32 hash) {
    return idx < ntextures && textures[idx].key == key && textures[idx].hash == hash;
}

const u8 *ps2_vk_texture_pixels(u32 idx) {
    return idx < ntextures ? __atomic_load_n(&textures[idx].rgba, __ATOMIC_ACQUIRE) : NULL;
}

void ps2_vk_texture_size(u32 idx, u32 *w, u32 *h) {
    *w = idx < ntextures ? textures[idx].w : 0u;
    *h = idx < ntextures ? textures[idx].h : 0u;
}

u32 ps2_vk_texture(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h) {
    size_t bytes = (size_t)w * h * 4u;
    u32 i, spare = 0xFFFFFFFFu;
    if (!vk_ready || !w || !h) return 0xFFFFFFFFu;
    for (s32 c = tex_first(key); c >= 0; c = tex_next[c]) {
        i = (u32)c;
        if (textures[i].key != key || textures[i].w != w || textures[i].h != h)
            continue;
        if (textures[i].hash == hash) {
            textures[i].rec_seq = tex_rec_seq;
            return i;
        }
        if (spare == 0xFFFFFFFFu && textures[i].rgba
            && (u32)(tex_rec_seq - textures[i].rec_seq) > 2u)
            spare = i;
    }
    if (spare != 0xFFFFFFFFu) {
        i = spare;
        memcpy(textures[i].rgba_alt, rgba, bytes);
        {   u8 *now_live = textures[i].rgba_alt;
            textures[i].rgba_alt = textures[i].rgba;
            __atomic_store_n(&textures[i].rgba, now_live, __ATOMIC_RELEASE);
        }
        textures[i].hash = hash;
        textures[i].rec_seq = tex_rec_seq;
        __atomic_add_fetch(&textures[i].generation, 1, __ATOMIC_RELEASE);
        return i;
    }
    if (ntextures >= VK_MAX_TEXTURES || tex_bytes + bytes * 2u > VK_TEX_BYTES) {
        static int said;
        if (!said) {
            said = 1;
            ps2_log("vk: decoded-texture store exhausted -- %u textures, "
                    "%.1f MB of %u MB, and a %ux%u one did not fit.  EVERY new "
                    "texture from here on will be dropped and the draws using "
                    "them will render untextured (white).",
                    ntextures, (double)tex_bytes / (1024.0 * 1024.0),
                    (unsigned)(VK_TEX_BYTES >> 20), w, h);
        }
        return 0xFFFFFFFFu;
    }
    i = ntextures++;
    textures[i].key = key;
    textures[i].hash = hash;
    textures[i].w = w;
    textures[i].h = h;
    textures[i].rgba = (u8 *)malloc(bytes);
    textures[i].rgba_alt = (u8 *)malloc(bytes);
    if (!textures[i].rgba || !textures[i].rgba_alt) {
        free(textures[i].rgba);
        free(textures[i].rgba_alt);
        textures[i].rgba = textures[i].rgba_alt = NULL;
        ntextures--;
        return 0xFFFFFFFFu;
    }
    memcpy(textures[i].rgba, rgba, bytes);
    tex_index_add(i);
    textures[i].uploaded_gen = 0;
    textures[i].rec_seq = tex_rec_seq;
    __atomic_store_n(&textures[i].generation, 1u, __ATOMIC_RELEASE);
    tex_bytes += bytes * 2u;
    return i;
}

void ps2_vk_invalidate(u32 base, u32 size) {
    (void)base; (void)size;
}

int ps2_vk_present(u32 disp_x, u32 disp_y, u32 disp_w, u32 disp_h, u32 rt) {
    int replace;
    if (!vk_ready) return 0;
    if (ps2_vk_before_record) ps2_vk_before_record();
    {
        static u64 calls;
        if ((calls++ % 600ull) == 0ull)
            ps2_log("alive: present #%llu  dropped=%llu  closed=%d "
                    "frame_done=%d requested=%d", (unsigned long long)calls,
                    (unsigned long long)stat_dropped, window_closed,
                    frame_done, frame_requested);
    }
    if (window_closed) return 0;

    {
        static long shot_every = -1;
        static u64 shot_calls;
        if (shot_every < 0) {
            const char *e = getenv("PS2_SHOT_EVERY");
            shot_every = e ? strtol(e, NULL, 0) : 0;
        }
        if (shot_every > 0 && (shot_calls % (u64)shot_every) == 0) {
            char nm[64];
            snprintf(nm, sizeof nm, "out/shot_%06llu.ppm",
                     (unsigned long long)shot_calls);
            ps2_vk_screenshot(nm);
        }
        shot_calls++;
    }

    pthread_mutex_lock(&list_lock);
    if (frame_requested && renderer_running > 0 && ps2_vk_lockstep) {
        while (frame_requested && renderer_running > 0 && !window_closed)
            pthread_cond_wait(&list_cv, &list_lock);
    }
    replace = frame_requested && renderer_running > 0;
    if (replace) {
        stat_dropped++;
        if (++stall_drops == 300u) {
            ps2_log("vk: renderer has not taken a field in %u tries -- it "
                    "also owns the window's events, so Esc and the close "
                    "button cannot be seen.  Ending the run so the summary "
                    "is written.", stall_drops);
            ps2_request_exit();
        }
    } else {
        stall_drops = 0;
    }
    {
        static int tf = -1;
        static u64 p_kick, p_adc, p_prim;
        if (tf < 0) { const char *e = getenv("PS2_TRACE_FIELD");
                      tf = e ? (int)strtol(e, NULL, 0) : 0; }
        if (tf > 0) {
            tf--;
            ps2_log("field %-6u kicked=%-7llu adc=%-7llu prims=%-7llu "
                    "verts=%-7u draws=%u", stat_frames,
                    (unsigned long long)(ps2_gs_vtx_draw - p_kick),
                    (unsigned long long)(ps2_gs_vtx_adc - p_adc),
                    (unsigned long long)(ps2_gs_prim_seq - p_prim),
                    rec_nverts, rec_ndraws);
        }
        p_kick = ps2_gs_vtx_draw; p_adc = ps2_gs_vtx_adc;
        p_prim = ps2_gs_prim_seq;
    }
    pub_verts  = rec_verts;
    pub_draws  = rec_draws;
    pub_nverts = rec_nverts;
    pub_ndraws = rec_ndraws;
    pub_mesh   = rec_mesh;
    {   int next = replace ? pub_slot : (0 + 1 + 2) - pub_slot - rec_slot;
        pub_slot = rec_slot;
        rec_slot = next;
    }
    rec_verts  = slot_verts[rec_slot];
    rec_draws  = slot_draws[rec_slot];
    rec_nverts = 0;
    rec_ndraws = 0;
    rec_mesh   = &slot_mesh[rec_slot];
    rec_mesh->nrecs = rec_mesh->nidx = rec_mesh->nblocks = 0;
    if (disp_w) present_w = disp_w;
    if (disp_h) present_h = disp_h;
    present_x = disp_x;
    present_y = disp_y;
    present_blank = rt == PS2_VK_PRESENT_BLANK;
    present_rt = rt < RT_SLOTS ? rt : 0;
    if (env_show_rt == -1) env_show_rt = env_flag("PS2_SHOW_RT");
    if (env_show_rt >= 0) { present_rt = (u32)env_show_rt % RT_SLOTS; present_blank = 0; }
    if (!replace) tex_rec_seq++;
    frame_requested = 1;
    frame_done = 0;
    pthread_cond_broadcast(&list_cv);
    if (ps2_vk_lockstep) {
        while (!frame_done && renderer_running > 0 && !window_closed)
            pthread_cond_wait(&list_cv, &list_lock);
    }
    pthread_mutex_unlock(&list_lock);
    if (rec_overflow) {
        ps2_log("vk: display list overflowed (%u verts / %u draws); "
                "the frame was truncated", VK_MAX_VERTS, VK_MAX_DRAWS);
        rec_overflow = 0;
    }
    return !window_closed;
}

int ps2_video_frame(void) {
    if (!vk_ready) return 0;
    return ps2_vk_present(present_x, present_y, present_w, present_h,
                          present_rt);
}

void ps2_vk_stats(u64 *f, u64 *d, u64 *v, u64 *t) {
    if (f) *f = stat_frames;
    if (d) *d = stat_draws;
    if (v) *v = stat_verts;
    if (t) *t = stat_texuploads;
}

void ps2_video_report(void) {
    if (!vk_ready) { ps2_log("vk: renderer was not active"); return; }
    ps2_log("vk: %llu frames, %llu draw calls, %llu vertices, "
            "%llu texture uploads (%u cached, %.1f MB)",
            (unsigned long long)stat_frames, (unsigned long long)stat_draws,
            (unsigned long long)stat_verts, (unsigned long long)stat_texuploads,
            ntextures, (double)tex_bytes / (1024.0 * 1024.0));
    {   VkDeviceSize used = 0;
        for (u32 i = 0; i < ntexmem; i++) used += texmem[i].used;
        ps2_log("vk: %u pipelines (%llu built ahead from earlier sessions, cache %s); "
                "texture images in %u blocks of %llu MB (%.1f MB used), %llu with "
                "memory of their own", npipes, (unsigned long long)stat_pipe_warm,
                pipe_cache ? "on" : "off", ntexmem,
                (unsigned long long)(TEXMEM_BLOCK >> 20), (double)used / (1024.0 * 1024.0),
                (unsigned long long)stat_texmem_own);
    }
    if (stat_selfreads)
        ps2_log("vk: %llu draws read back the target they were drawing into",
                (unsigned long long)stat_selfreads);
    if (stat_native_draws || stat_native_vis_none)
        ps2_log("vk: %llu native draws (rn.h claims), %llu of them with a "
                "visibility reduction, %llu keeping the last one (no sample that "
                "frame); %llu passes not drawn before any sample",
                (unsigned long long)stat_native_draws,
                (unsigned long long)stat_native_vis,
                (unsigned long long)stat_native_vis_kept,
                (unsigned long long)stat_native_vis_none);
    if (stat_mesh_draws || stat_mesh_overflow)
        ps2_log("vk: %llu native mesh draws (mesh.vert), %llu recorded with the native "
                "material (mesh.frag); %llu batches refused for arena space",
                (unsigned long long)stat_mesh_draws, (unsigned long long)stat_mesh_mat,
                (unsigned long long)stat_mesh_overflow);
    if (stat_ov_frames || stat_ov_inplace)
        ps2_log("vk: 2D overlay composed on %llu frames at %ux, %llu draws lifted "
                "onto it, %llu native 2D draws kept in place because the frame "
                "wrote or read slot 0 after them%s",
                (unsigned long long)stat_ov_frames, ov_k,
                (unsigned long long)stat_ov_draws,
                (unsigned long long)stat_ov_inplace,
                ov_lists[0].overflow || ov_lists[1].overflow
                    ? "; an overlay list overflowed" : "");
    if (stat_ov_rgbreads)
        ps2_log("vk: 2D overlay -- %llu copies of slot 0's colour into another target "
                "kept the draws before them in place", (unsigned long long)stat_ov_rgbreads);
    if (stat_dropped)
        ps2_log("vk: %llu fields dropped (replaced by a newer one before the "
                "renderer took them)", (unsigned long long)stat_dropped);
}

static int write_ppm(const char *path, const u8 *px, u32 ox, u32 oy,
                     u32 w, u32 h) {
    FILE *fp = fopen(path, "wb");
    u32 x, y;
    if (!fp) return -1;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        const u8 *row = px + (size_t)(y + oy) * read_w * 4 + (size_t)ox * 4;
        for (x = 0; x < w; x++) {
            fputc(row[x * 4 + 0], fp);
            fputc(row[x * 4 + 1], fp);
            fputc(row[x * 4 + 2], fp);
        }
    }
    fclose(fp);
    return 0;
}

static int write_alpha_pgm(const char *path, const u8 *px, u32 w, u32 h) {
    FILE *fp = fopen(path, "wb");
    u32 x, y;
    if (!fp) return -1;
    fprintf(fp, "P5\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        const u8 *row = px + (size_t)y * read_w * 4;
        for (x = 0; x < w; x++) fputc(row[x * 4 + 3], fp);
    }
    fclose(fp);
    return 0;
}

static VkDeviceSize read_off[RT_SLOTS];

static int readback_targets(int all) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = {0};
    VkCommandBufferBeginInfo bi = {0};
    VkSubmitInfo si = {0};
    u32 iw = rt_w * rt_scale, ih = rt_h * rt_scale;
    VkDeviceSize one = (VkDeviceSize)iw * ih * 4u, need = 0;
    if (!read_w || !rt_w || !rt_h) return -1;
    if (vkDeviceWaitIdle(dev) != VK_SUCCESS) return -1;
    for (u32 ti = 0; ti < RT_SLOTS; ti++) {
        read_off[ti] = need;
        if (rts[ti].created && (all || ti == read_slot)) need += one;
    }
    if (!need) return -1;
    read_w = iw;
    read_h = ih;
    if (need > read_capacity) {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void *ptr = NULL;
        if (make_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &buf, &mem, &ptr) != 0) {
            if (ptr) vkUnmapMemory(dev, mem);
            if (buf) vkDestroyBuffer(dev, buf, NULL);
            if (mem) vkFreeMemory(dev, mem, NULL);
            return -1;
        }
        if (read_ptr) vkUnmapMemory(dev, read_mem);
        if (read_buf) vkDestroyBuffer(dev, read_buf, NULL);
        if (read_mem) vkFreeMemory(dev, read_mem, NULL);
        read_buf = buf; read_mem = mem; read_ptr = ptr; read_capacity = need;
    }

    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return -1;
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) goto fail;
    for (u32 ti = 0; ti < RT_SLOTS; ti++) {
        vk_target *t = &rts[ti];
        if (!t->created || (!all && ti != read_slot)) continue;
        VkImageLayout previous = t->layout;
        if (previous == VK_IMAGE_LAYOUT_UNDEFINED) {
            memset((u8 *)read_ptr + read_off[ti], 0, (size_t)one);
            continue;
        }
        barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
            previous, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {0};
        copy.bufferOffset = read_off[ti];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = (VkExtent3D){iw, ih, 1};
        vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            read_buf, 1, &copy);
        barrier(cmd, t->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, previous,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    VkMemoryBarrier host = {0};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &host, 0, NULL, 0, NULL);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) goto fail;
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) goto fail;
    if (vkQueueWaitIdle(queue) != VK_SUCCESS) {
        ps2_log("vk: requested readback wait failed");
        return -1;
    }
    vkFreeCommandBuffers(dev, cmdpool, 1, &cmd);
    return 0;
fail:
    vkFreeCommandBuffers(dev, cmdpool, 1, &cmd);
    ps2_log("vk: requested readback failed");
    return -1;
}

void ps2_vk_statecap(const char *dir) {
    char p[400];
    u32 ti;
    FILE *f;

    if (!vk_ready) return;
    pthread_mutex_lock(&list_lock);
    while (!frame_done && renderer_running > 0 && !window_closed)
        pthread_cond_wait(&list_cv, &list_lock);
    pthread_mutex_lock(&gpu_lock);
    if (readback_targets(1) != 0) {
        pthread_mutex_unlock(&gpu_lock);
        pthread_mutex_unlock(&list_lock);
        return;
    }

    snprintf(p, sizeof p, "%s/vk.txt", dir);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "# Renderer state at the frame after the snapshot was asked\n"
                   "# for.  Every live target is also dumped as\n"
                   "# rt_<slot>_<w>x<h>.ppm (P6, top-down).  `displayed` marks\n"
                   "# the one the DISPLAY registers were selecting.\n\n");
        fprintf(f, "targets %ux%u at %ux  readback %ux%u  displayed slot %u\n",
                rt_w, rt_h, rt_scale, read_w, read_h, read_slot);
        fprintf(f, "present region x=%u y=%u %ux%u\n\n",
                present_x, present_y, present_w, present_h);
        fprintf(f, "== target slots ==\n");
        for (ti = 0; ti < RT_SLOTS; ti++)
            if (rts[ti].created)
                fprintf(f, "  slot %-2u  created%s\n", ti,
                        ti == read_slot ? "   <- displayed" : "");
        fclose(f);
    }

    for (ti = 0; ti < RT_SLOTS; ti++) {
        if (!rts[ti].created) continue;
        snprintf(p, sizeof p, "%s/rt_%02u_%ux%u.ppm", dir, ti, read_w, read_h);
        write_ppm(p, (const u8 *)read_ptr + read_off[ti], 0, 0, read_w, read_h);
    }

    if (depth_ptr && depth_w) {
        const float *d = (const float *)depth_ptr;
        u32 n = depth_w * depth_h, k;
        float lo = 1e30f, hi = -1e30f;
        for (k = 0; k < n; k++) { if (d[k] < lo) lo = d[k]; if (d[k] > hi) hi = d[k]; }
        snprintf(p, sizeof p, "%s/depth_%ux%u.pgm", dir, depth_w, depth_h);
        f = fopen(p, "wb");
        if (f) {
            float span = (hi > lo) ? (hi - lo) : 1.0f;
            fprintf(f, "P5\n%u %u\n255\n", depth_w, depth_h);
            for (k = 0; k < n; k++) {
                int b = (int)((d[k] - lo) / span * 255.0f + 0.5f);
                fputc(b < 0 ? 0 : b > 255 ? 255 : b, f);
            }
            fclose(f);
        }
    }
    pthread_mutex_unlock(&gpu_lock);
    pthread_mutex_unlock(&list_lock);
}

static VkBuffer ovr_buf;
static VkDeviceMemory ovr_mem;
static void *ovr_ptr;
static VkDeviceSize ovr_cap;
static int readback_overlay(void) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = {0};
    VkCommandBufferBeginInfo bi = {0};
    VkSubmitInfo si = {0};
    u32 iw = ov_cw * ov_k, ih = ov_ch * ov_k;
    VkDeviceSize need = (VkDeviceSize)iw * ih * 4u;
    VkImageLayout prev = ov_comp.layout;
    if (!ov_comp.created || prev == VK_IMAGE_LAYOUT_UNDEFINED) return -1;
    if (vkDeviceWaitIdle(dev) != VK_SUCCESS) return -1;
    if (need > ovr_cap) {
        if (ovr_ptr) vkUnmapMemory(dev, ovr_mem);
        if (ovr_buf) vkDestroyBuffer(dev, ovr_buf, NULL);
        if (ovr_mem) vkFreeMemory(dev, ovr_mem, NULL);
        ovr_buf = VK_NULL_HANDLE; ovr_mem = VK_NULL_HANDLE; ovr_ptr = NULL; ovr_cap = 0;
        if (make_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &ovr_buf, &ovr_mem, &ovr_ptr) != 0) return -1;
        ovr_cap = need;
    }
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return -1;
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    barrier(cmd, ov_comp.color, VK_IMAGE_ASPECT_COLOR_BIT, prev,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    {   VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = (VkExtent3D){iw, ih, 1};
        vkCmdCopyImageToBuffer(cmd, ov_comp.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ovr_buf, 1, &copy); }
    barrier(cmd, ov_comp.color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prev,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    vkEndCommandBuffer(cmd);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS
        || vkQueueWaitIdle(queue) != VK_SUCCESS) {
        vkFreeCommandBuffers(dev, cmdpool, 1, &cmd);
        return -1;
    }
    vkFreeCommandBuffers(dev, cmdpool, 1, &cmd);
    return 0;
}

int ps2_vk_screenshot(const char *path) {
    FILE *fp;
    const u8 *px = (const u8 *)read_ptr;
    u32 x, y, w, h, ox, oy;
    if (!vk_ready) return -1;
    pthread_mutex_lock(&list_lock);
    pthread_mutex_lock(&gpu_lock);
    if (ov_presented && !getenv("PS2_SHOT_NO_OVERLAY") && readback_overlay() == 0) {
        u32 k = ov_k, iw = ov_cw * k;
        u32 pw = present_w * k, ph = present_h * k;
        ox = present_x * k; oy = present_y * k;
        w = pw && pw <= iw ? pw : iw;
        h = ph && ph <= ov_ch * k ? ph : ov_ch * k;
        fp = fopen(path, "wb");
        if (fp) {
            fprintf(fp, "P6\n%u %u\n255\n", w, h);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const u8 *p = (const u8 *)ovr_ptr + ((size_t)(oy + y) * iw + ox + x) * 4u;
                    fputc(p[0], fp); fputc(p[1], fp); fputc(p[2], fp);
                }
            fclose(fp);
            ps2_log("vk: frame written to %s (%ux%u, with the 2D overlay at %ux)",
                    path, w, h, k);
        }
        pthread_mutex_unlock(&gpu_lock);
        pthread_mutex_unlock(&list_lock);
        return fp ? 0 : -1;
    }
    if (readback_targets(getenv("PS2_DUMP_RTS") != NULL) != 0) {
        pthread_mutex_unlock(&gpu_lock);
        pthread_mutex_unlock(&list_lock);
        return -1;
    }
    if (swap_ptr && swap_w && getenv("PS2_DUMP_SWAP")) {
        u32 sw2 = read_w;
        read_w = swap_w;
        if (write_ppm("ps2_swapchain.ppm", (const u8 *)swap_ptr, 0, 0,
                      swap_w, swap_h) == 0)
            ps2_log("vk: swapchain written to ps2_swapchain.ppm (%ux%u)",
                    swap_w, swap_h);
        read_w = sw2;
    }
    if (depth_ptr && depth_w && want_depth_dump()) {
        const float *d = (const float *)depth_ptr;
        float lo = 1e30f, hi = -1e30f;
        u32 n = depth_w * depth_h, k;
        FILE *dp;
        for (k = 0; k < n; k++) { if (d[k] < lo) lo = d[k]; if (d[k] > hi) hi = d[k]; }
        ps2_log("vk: depth buffer of target 0 spans %.6f .. %.6f", (double)lo, (double)hi);
        dp = fopen("ps2_depth.pgm", "wb");
        if (dp) {
            float span = (hi > lo) ? (hi - lo) : 1.0f;
            fprintf(dp, "P5\n%u %u\n255\n", depth_w, depth_h);
            for (k = 0; k < n; k++) {
                float v = (d[k] - lo) / span;
                int b = (int)(v * 255.0f + 0.5f);
                fputc(b < 0 ? 0 : b > 255 ? 255 : b, dp);
            }
            fclose(dp);
            ps2_log("vk: depth written to ps2_depth.pgm (%ux%u)", depth_w, depth_h);
        }
    }
    if (getenv("PS2_DUMP_RTS")) {
        u32 ti;
        for (ti = 0; ti < RT_SLOTS; ti++) {
            char nm[256];
            if (!rts[ti].created) continue;
            snprintf(nm, sizeof nm, "ps2_rt%u.ppm", ti);
            if (write_ppm(nm, (const u8 *)read_ptr + read_off[ti],
                          0, 0, read_w, read_h) == 0)
                ps2_log("vk: target %u written to %s (%ux%u)%s", ti, nm,
                        read_w, read_h, ti == read_slot ? "  <- displayed" : "");
            if (getenv("PS2_DUMP_ALPHA")) {
                snprintf(nm, sizeof nm, "ps2_rt%u_alpha.pgm", ti);
                write_alpha_pgm(nm, (const u8 *)read_ptr + read_off[ti],
                                read_w, read_h);
            }
        }
    }
    px = (const u8 *)read_ptr + read_off[read_slot];
    ox = present_x * rt_scale;
    oy = present_y * rt_scale;
    if (getenv("PS2_FULL_RT")) { w = read_w; h = read_h; ox = oy = 0; }
    else {
        u32 pw = present_w * rt_scale, ph = present_h * rt_scale;
        w = pw && pw <= read_w ? pw : read_w;
        h = ph && ph <= read_h ? ph : read_h;
        if (ox + w > read_w) ox = read_w - w;
        if (oy + h > read_h) oy = read_h - h;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        pthread_mutex_unlock(&gpu_lock);
        pthread_mutex_unlock(&list_lock);
        return -1;
    }
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        const u8 *row = px + (size_t)(y + oy) * read_w * 4 + (size_t)ox * 4;
        for (x = 0; x < w; x++) {
            fputc(row[x * 4 + 0], fp);
            fputc(row[x * 4 + 1], fp);
            fputc(row[x * 4 + 2], fp);
        }
    }
    fclose(fp);
    ps2_log("vk: frame written to %s (%ux%u)", path, w, h);
    pthread_mutex_unlock(&gpu_lock);
    pthread_mutex_unlock(&list_lock);
    return 0;
}

void ps2_video_shutdown(void) {
    if (!renderer_running) return;
    pthread_mutex_lock(&list_lock);
    renderer_running = -1;
    pthread_cond_broadcast(&list_cv);
    pthread_mutex_unlock(&list_lock);
    pthread_join(renderer_thread, NULL);
    if (dev) vkDeviceWaitIdle(dev);
}
