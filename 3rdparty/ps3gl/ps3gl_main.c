/* ps3gl_main.c -- GL-to-RSX layer: initialization, shutdown, frame bracket. */

#include "ps3gl.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h> // usleep, used by the vertex-ring fence wait below

/* __gettime() (mftb) and sysGetTimebaseFrequency() for ps3gl_ticks below.
 * Same diagnostic wrap as ps3gl.h uses for the rsx headers -- these vendor
 * headers define LV2 syscall wrappers as foo() rather than foo(void). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#include <ppu-asm.h>
#include <sys/systime.h>
#pragma GCC diagnostic pop

/* ps3gl is linked into the engine (see engine/wscript), so this is the engine's
 * own Con_Printf -- which mirrors to the UDP debug sink through
 * Sys_PrintStdout's XASH_PS3 branch. Declared by hand rather than by pulling in
 * common.h, which would drag the whole engine header chain into a 3rdparty TU. */
extern void Con_Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Same "declare just what we need" approach as Con_Printf above -- avoids
 * pulling engine/platform/platform.h's whole header chain into a 3rdparty TU.
 * Real wall-clock double, already implemented for PS3 in sys_ps3.c. */
extern double Platform_DoubleTime(void);

/* Profiling accumulators for the frame-time telemetry window below -- pure
 * instrumentation, no behavior change. Answers two open questions before any
 * optimization is attempted: (1) how much of avg frame time is the CPU
 * blocked in ps3gl_begin_frame's vring fence wait vs. real GPU/engine work
 * elsewhere, and (2) how many real draw calls (glEnd flushes) actually
 * happen per frame, since the existing ps3gl_frame_draw_count in
 * ps3gl_vertices.c is only ever incremented from glDrawElements, which xash
 * never calls -- it undercounts every real draw, all of which go through
 * immediate-mode glBegin/glEnd. */
double ps3gl_frame_fence_wait_time = 0.0;
uint32_t ps3gl_frame_flush_count = 0;
/* Vertices submitted across all flushes this frame -- divided by
 * ps3gl_frame_flush_count in the window report below to get avg
 * vertices/flush, which is what actually distinguishes "many small
 * immediate-mode batches paying fixed per-flush overhead" (fixable by
 * batching) from "same geometry total, just counted differently" (not
 * fixable without a bigger renderer change) when flush count alone
 * correlates with a frame-time regression. */
uint32_t ps3gl_frame_flush_vertex_count = 0;
/* GL_POLYGON draws merged into a deferred batch instead of flushed
 * individually this frame (ps3gl_vertices.c). The only way to distinguish
 * "batching is working" from "the state-clean gate never passes" -- given
 * how small the predicted saving already is, an unverifiable no-op is the
 * most likely failure mode of the whole batching mechanism. */
uint32_t ps3gl_frame_batched_poly_count = 0;

/* glDrawElements direct-bind vs interleave-copy fallback (ps3gl_draw.c).
 * The `submit` studio phase measured 2.66ms/frame and the whole host-ring
 * work below is premised on those draws taking the fallback -- which was
 * INFERRED from ref_gl's arrays living in un-IO-mapped BSS, never measured.
 * These settle it: fallback_binds near zero would mean the premise was
 * wrong and the cost is somewhere else entirely. */
uint32_t ps3gl_frame_direct_binds = 0;
uint32_t ps3gl_frame_fallback_binds = 0;
uint32_t ps3gl_frame_fallback_verts = 0;

/* Engine-side (ref/gl/gl_rsurf.c) dlight CPU-cost counters, reported once
 * per frame via ps3gl_report_dlight_cost() below -- test whether
 * dynamic-light lightmap rebuild cost is an independent frame-time driver
 * from RSX flush count. NOT read as extern data: --static-linking's
 * `objcopy -G lib_ref_gl_exports` localizes every symbol in ref_gl.o except
 * GetRefAPI (see engine/wscript's ps3gl-linkage comment), so a data symbol
 * defined in gl_rsurf.c would be invisible here -- only an ordinary
 * undefined-function-call boundary (same mechanism as ref_gl's pglFoo()
 * calls resolving against ps3gl's real glFoo() at final link) crosses it. */
static uint32_t g_dlight_surface_count = 0;
static uint32_t g_dlight_luxel_count = 0;

/* Bumped once per ps3gl_begin_frame. Only consumer is the stream-VBO
 * re-specify check in ps3gl_glapi.c, which needs to tell "twice this frame"
 * apart from "once per frame, twice in a row". */
static uint32_t g_frame_index = 0;

uint32_t ps3gl_frame_index(void)
{
    return g_frame_index;
}

void ps3gl_report_dlight_cost(uint32_t surface_count, uint32_t luxel_count)
{
    g_dlight_surface_count = surface_count;
    g_dlight_luxel_count   = luxel_count;
}

/* Time the CPU spends blocked in vid_ps3.c's PS3_RSX_WaitForFreeBuffer -- the
 * ONE wait in the frame that was never instrumented, and the only place a
 * GPU-bound CPU actually blocks. The vring fence wait below runs AFTER it
 * (PS3_GL_BeginFrame calls WaitForFreeBuffer, then ps3gl_begin_frame), so a
 * GPU that is the real bottleneck burns its slack here first and leaves the
 * fence reading ~0% -- which is exactly what every previous session measured
 * and misread as "never a GPU wait". With this, frame time is fully
 * accounted: total = flip wait + vring fence + everything else.
 *
 * sleeps is the usleep(100) iteration count from the same loop. If measured
 * time / sleeps is far above 100us, LV2 usleep granularity is itself
 * inflating frames and the fix is a short spin before sleeping -- a different
 * bug from "the GPU is slow", and indistinguishable without this second number. */
static double   g_flip_wait_time = 0.0;
static uint32_t g_flip_wait_sleeps = 0;

void ps3gl_report_flip_wait(double seconds, uint32_t sleeps)
{
    g_flip_wait_time   += seconds;
    g_flip_wait_sleeps += sleeps;
}

/* Coarse CPU phase split, fed once per frame from engine/common/host.c. The
 * flip-wait measurement above proved the ~50fps floor is CPU-bound (0.00ms
 * spent waiting on the GPU in every slow window), which means the next
 * question is simply WHICH CPU work -- and nothing in this engine build
 * answered that (no host_speeds in this Xash version). See public/ps3_diag.h. */
static double g_phase_input = 0.0, g_phase_server = 0.0, g_phase_client = 0.0;
static double g_phase_render = 0.0, g_phase_sound = 0.0, g_phase_total = 0.0;

/* Breakdown of the server phase itself. The 2026-08-05 rope measurement put
 * the OpFor rope-area cost in `server` (10.50 -> 17.86ms, 64% of frame) but
 * could not say which part. `think` is the game DLL's per-entity think,
 * `link` is SV_LinkEdict's areanode relink; server minus both is the rest
 * (packets, client commands). See public/ps3_diag.h. */
static double g_phase_sv_think = 0.0, g_phase_sv_link = 0.0;
static unsigned int g_phase_sv_think_calls = 0, g_phase_sv_link_calls = 0;

/* Round 2: think/link measured flat and tiny while the leftover `rest`
 * carried the entire framerate swing (10.4 -> 20.8ms). These are the three
 * top-level calls inside Host_ServerFrame that `rest` consists of. Note
 * gameframe CONTAINS think and link -- they are nested inside SV_Physics. */
static double g_phase_sv_readpackets = 0.0, g_phase_sv_gameframe = 0.0;
static double g_phase_sv_sendmsgs = 0.0;

/* Round 3: sendmsgs measured 16.15ms of a 20.4ms server frame while looking
 * AWAY from the ropes -- snapshot transmission, not physics, and not view
 * dependent. These split it into the per-entity visibility walk vs the
 * delta encode. */
static double g_phase_sv_addents = 0.0, g_phase_sv_emit = 0.0;
static unsigned int g_phase_sv_addents_calls = 0, g_phase_sv_fullpack_calls = 0;

/* Round 4: emit is ~31us/entity over ~90 fields. These say whether field
 * suppression works (written/visited) and whether the game DLL's encoder
 * callbacks survived the delta table rebuilds (with_encoder; 0 = wiped). */
static unsigned int g_delta_visited = 0, g_delta_written = 0;
static unsigned int g_delta_entities = 0, g_delta_with_encoder = 0;

/* Round 5: SV_FindBestBaseline's backward search, the work the delta field
 * counters above do not see. tests x fields/ent is the real compare count. */
static unsigned int g_bl_calls = 0, g_bl_tests = 0, g_bl_iters = 0;
static unsigned int g_bl_newents = 0, g_bl_num_instanced = 0;

/* Breakdown of the render phase itself, fed from ref_gl's R_RenderScene.
 * ACCUMULATED, not assigned: R_RenderScene runs more than once per frame when
 * a mirror/portal/water-reflection pass is active, and a pass that only shows
 * up on some frames is exactly the kind of cost this is hunting. Zeroed once
 * per frame below, alongside every other per-frame counter. */
static double g_scene_setup = 0.0, g_scene_world = 0.0;
static double g_scene_ents = 0.0, g_scene_water = 0.0;
static uint32_t g_scene_passes = 0;

void ps3gl_report_scene_phases(double setup, double world, double entities,
                               double water)
{
    g_scene_setup  += setup;
    g_scene_world  += world;
    g_scene_ents   += entities;
    g_scene_water  += water;
    g_scene_passes++;
}

/* PPE time base. One mftb, no syscall -- see the header comment on
 * ps3gl_ticks. PSL1GHT's __gettime() spins only on the degenerate tb==0
 * reading, which cannot persist. The frequency is a syscall, so it is queried
 * once and cached; 79.8 MHz on retail hardware, but read it rather than
 * hardcode it. */
uint64_t ps3gl_ticks(void)
{
    return __gettime();
}

double ps3gl_ticks_to_sec(uint64_t ticks)
{
    static double inv_freq = 0.0;

    if (inv_freq == 0.0) {
        uint64_t freq = sysGetTimebaseFrequency();
        if (freq == 0) return 0.0;
        inv_freq = 1.0 / (double)freq;
    }
    return (double)ticks * inv_freq;
}

/* Entity-phase and studio-phase splits (see the header). Ticks, not seconds:
 * the conversion is one multiply and belongs in the once-per-5s report, not in
 * the accumulate path that runs per entity. Accumulated for the same reason as
 * the scene phases -- a mirror/portal pass re-runs R_DrawEntitiesOnList. */
static uint64_t g_ent_studio = 0, g_ent_brush = 0, g_ent_sprite = 0;
static uint64_t g_ent_efx = 0, g_ent_clienttri = 0, g_ent_viewmodel = 0;
static uint64_t g_stu_client = 0, g_stu_entlight = 0, g_stu_skin = 0;
static uint64_t g_stu_light = 0, g_stu_build = 0;
static uint64_t g_stu_fill = 0, g_stu_submit = 0;
static uint64_t g_stu_cxform = 0, g_stu_cbones = 0, g_stu_cevents = 0;
static uint32_t g_stu_models = 0;

void ps3gl_report_entity_phases(uint64_t studio, uint64_t brush, uint64_t sprite,
                                uint64_t efx, uint64_t clienttri, uint64_t viewmodel)
{
    g_ent_studio    += studio;
    g_ent_brush     += brush;
    g_ent_sprite    += sprite;
    g_ent_efx       += efx;
    g_ent_clienttri += clienttri;
    g_ent_viewmodel += viewmodel;
}

void ps3gl_report_studio_phases(uint64_t client, uint64_t entlight, uint64_t skin,
                                uint64_t light, uint64_t build, uint64_t fill,
                                uint64_t submit, uint32_t models)
{
    g_stu_client   += client;
    g_stu_entlight += entlight;
    g_stu_skin     += skin;
    g_stu_light    += light;
    g_stu_build    += build;
    g_stu_fill     += fill;
    g_stu_submit   += submit;
    g_stu_models   += models;
}

void ps3gl_report_studio_client(uint64_t xform, uint64_t bones, uint64_t events)
{
    g_stu_cxform  += xform;
    g_stu_cbones  += bones;
    g_stu_cevents += events;
}

void ps3gl_report_frame_phases(double input, double server, double client,
                               double render, double sound, double total)
{
    g_phase_input  = input;
    g_phase_server = server;
    g_phase_client = client;
    g_phase_render = render;
    g_phase_sound  = sound;
    g_phase_total  = total;
}

void ps3gl_report_server_phases(double think, double link,
                                unsigned int think_calls, unsigned int link_calls)
{
    g_phase_sv_think       = think;
    g_phase_sv_link        = link;
    g_phase_sv_think_calls = think_calls;
    g_phase_sv_link_calls  = link_calls;
}

void ps3gl_report_server_split(double readpackets, double gameframe, double sendmsgs)
{
    g_phase_sv_readpackets = readpackets;
    g_phase_sv_gameframe   = gameframe;
    g_phase_sv_sendmsgs    = sendmsgs;
}

void ps3gl_report_snapshot_split(double addents, double emit,
                                 unsigned int addents_calls, unsigned int fullpack_calls)
{
    g_phase_sv_addents       = addents;
    g_phase_sv_emit          = emit;
    g_phase_sv_addents_calls = addents_calls;
    g_phase_sv_fullpack_calls = fullpack_calls;
}

void ps3gl_report_delta_fields(unsigned int visited, unsigned int written,
                               unsigned int entities, unsigned int with_encoder)
{
    g_delta_visited      = visited;
    g_delta_written      = written;
    g_delta_entities     = entities;
    g_delta_with_encoder = with_encoder;
}

void ps3gl_report_baseline(unsigned int calls, unsigned int tests, unsigned int iters,
                           unsigned int newents, unsigned int num_instanced)
{
    g_bl_calls         = calls;
    g_bl_tests         = tests;
    g_bl_iters         = iters;
    g_bl_newents       = newents;
    g_bl_num_instanced = num_instanced;
}

/* Texture-upload telemetry -- answers whether occasional stutter/fps dips
 * correlate with synchronous, scalar CPU texture work (new/resized textures,
 * lightmap sub-uploads) rather than genuine GPU/scene-complexity load. See
 * ps3gl_textures.c's glTexImage2D/glTexSubImage2D/ps3gl_defer_texture_free. */
uint32_t ps3gl_frame_tex_alloc_count = 0;
uint32_t ps3gl_frame_subimage_count = 0;
uint32_t ps3gl_frame_subimage_pixels = 0;
uint32_t ps3gl_frame_defer_overflow_count = 0;

/* Mip-activation telemetry (ps3gl_textures.c). t->num_levels was pinned at
 * 0/1 forever before this fix -- these answer, on real hardware, whether
 * the engine ever hands ps3gl level>0 data at all (mip_uploads), and
 * whether mipped textures actually reach the RSX bind with maxlod>0
 * (mipped_binds/tex_binds), rather than trusting the CPU-side fix alone. */
uint32_t ps3gl_frame_mip_uploads = 0;
uint32_t ps3gl_frame_mipped_binds = 0;
uint32_t ps3gl_frame_tex_binds = 0;

void ps3gl_log(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    Con_Printf("%s", msg);
}

ps3gl_state_t *ps3gl_ptr = NULL;

gcmContextData *ps3gl_get_ctx(void)
{
    return ps3gl_ptr ? ps3gl_ptr->ctx : NULL;
}

int ps3gl_init(gcmContextData *ctx, uint32_t w, uint32_t h)
{
    if (!ctx) {
        ps3gl_log("[ps3gl] init failed: NULL gcm context\n");
        return 0;
    }

    if (!ps3gl_ptr) {
        ps3gl_ptr = (ps3gl_state_t *)malloc(sizeof(ps3gl_state_t));
        if (!ps3gl_ptr) {
            ps3gl_log("[ps3gl] init failed: out of memory (%u bytes)\n",
                      (unsigned)sizeof(ps3gl_state_t));
            return 0;
        }
    }
    memset(ps3gl_ptr, 0, sizeof(ps3gl_state_t));

    ps3gl.ctx        = ctx;
    ps3gl.screen_w = w;
    ps3gl.screen_h = h;
    ps3gl.dirty    = PS3GL_DIRTY_ALL;

    ps3gl.imm.color = ps3gl_pack_color(1.0f, 1.0f, 1.0f, 1.0f);

    ps3gl.rs.blend_enable       = 0;
    ps3gl.rs.blend_src          = GCM_ONE;
    ps3gl.rs.blend_dst          = GCM_ZERO;
    ps3gl.rs.alpha_test_enable  = 0;
    ps3gl.rs.alpha_func         = GCM_ALWAYS;
    ps3gl.rs.alpha_ref          = 0;
    ps3gl.rs.depth_test_enable  = 0;
    ps3gl.rs.depth_mask         = 1;
    ps3gl.rs.depth_func         = GCM_LESS;
    ps3gl.rs.cull_enable        = 0;
    ps3gl.rs.cull_face          = GCM_CULL_BACK;
    ps3gl.rs.front_face         = GCM_FRONTFACE_CCW;
    ps3gl.rs.scissor_enable     = 0;
    ps3gl.rs.scissor_x          = 0;
    ps3gl.rs.scissor_y          = 0;
    ps3gl.rs.scissor_w          = w;
    ps3gl.rs.scissor_h          = h;
    ps3gl.rs.vp_x               = 0;
    ps3gl.rs.vp_y               = 0;
    ps3gl.rs.vp_w               = w;
    ps3gl.rs.vp_h               = h;
    ps3gl.rs.depth_near         = 0.0f;
    ps3gl.rs.depth_far          = 1.0f;
    ps3gl.rs.color_mask_r       = 1;
    ps3gl.rs.color_mask_g       = 1;
    ps3gl.rs.color_mask_b       = 1;
    ps3gl.rs.color_mask_a       = 1;
    ps3gl.rs.polyoffset_fill    = 0;
    ps3gl.rs.polyoffset_factor  = 0.0f;
    ps3gl.rs.polyoffset_units   = 0.0f;
    ps3gl.rs.shade_model        = GCM_SHADE_MODEL_SMOOTH;
    ps3gl.rs.stencil_enable     = 0;
    ps3gl.rs.stencil_func       = GCM_ALWAYS;
    ps3gl.rs.stencil_ref        = 0;
    ps3gl.rs.stencil_mask       = 0xFFFFFFFF;
    ps3gl.rs.stencil_fail       = GCM_KEEP;
    ps3gl.rs.stencil_zfail      = GCM_KEEP;
    ps3gl.rs.stencil_zpass      = GCM_KEEP;
    ps3gl.rs.stencil_writemask  = 0xFFFFFFFF;
    ps3gl.rs.clear_color        = 0x00000000;
    ps3gl.rs.clear_depth        = 1.0f;
    ps3gl.rs.clear_stencil      = 0;

    for (int i = 0; i < PS3GL_MAX_TMUS; i++) {
        ps3gl.tmu[i].bound   = NULL;
        ps3gl.tmu[i].enabled = 0;
        ps3gl.tmu[i].texenv  = PS3GL_TENV_MODULATE;
        ps3gl.tmu[i].dirty   = 1;  /* force initial state push */
    }

    ps3gl.matrix_mode = GL_MODELVIEW;

    ps3gl_vring_init();
    ps3gl_hring_init();
    ps3gl_matrices_init();
    ps3gl_textures_init();
    ps3gl_shaders_init();
    ps3gl_states_init();
    ps3gl_buffers_init();

    ps3gl_log("[ps3gl] initialized (%ux%u)\n", (unsigned)w, (unsigned)h);
    return 1;
}

void ps3gl_shutdown(void)
{
    if (!ps3gl_ptr) return;

    ps3gl_shaders_shutdown();
    ps3gl_textures_shutdown();
    ps3gl_buffers_shutdown();
    ps3gl_vring_shutdown();
    ps3gl_hring_shutdown();

    free(ps3gl_ptr);
    ps3gl_ptr = NULL;

    ps3gl_log("[ps3gl] shutdown\n");
}

void ps3gl_begin_frame(void)
{
    if (!ps3gl_ptr) return;

    /* Defensive: a mid-frame Host_Error longjmp could skip a normal
     * ps3gl_end_frame (and its own polygon-batch flush) entirely, leaving a
     * batch pending across a frame boundary it was never fenced for. */
    ps3gl.imm.batch_active  = 0;
    ps3gl.imm.batch_nranges = 0;
    ps3gl.imm.count         = 0;

    /* Swap to the other vring segment and wait for the GPU to finish whatever it held
     * last time around -- same one-frame pipeline slack as before, just fenced per segment instead of the whole ring. */
    ps3gl.vring.cur_seg = (ps3gl.vring.cur_seg + 1) % PS3GL_VRING_SEGMENTS;
    {
        int seg = ps3gl.vring.cur_seg;
        volatile uint32_t *label = ps3gl.vring.fence_label[seg];
        double wait_start = Platform_DoubleTime();
        if (label) {
            int waited = 0;
            while (*label != ps3gl.vring.fence_val[seg] && waited < 20000) {
                usleep(10);
                waited++;
            }
            if (*label != ps3gl.vring.fence_val[seg])
                ps3gl_log("[ps3gl] WARNING: vring segment %d fence timed out (val=%u label=%u)\n",
                          seg, (unsigned)ps3gl.vring.fence_val[seg], (unsigned)*label);
        }
        ps3gl_frame_fence_wait_time += Platform_DoubleTime() - wait_start;
    }

    /* This fence proves the GPU has drained the frame(s) that could still be
     * sampling a texture unbound/resized last frame -- safe to actually
     * rsxFree() deferred textures now (see ps3gl_defer_texture_free). The same
     * fence covers VBO storage retired by glDeleteBuffersARB / a re-specify,
     * which the RSX fetches from exactly like a texture. */
    ps3gl_flush_deferred_texture_frees();
    ps3gl_buffers_begin_frame();

    g_frame_index++;

    ps3gl.vring.head = 0;
    ps3gl.hring.head = 0;

    ps3gl.dirty     = PS3GL_DIRTY_ALL;
    ps3gl.mv.dirty  = 1;
    ps3gl.proj.dirty = 1;
    ps3gl.active_shader = -1;
    /* Reassert VP once per frame too -- sysutil overlays (OSK, dialogs) can
     * rebind their own vertex program behind our active_vp cache's back. */
    ps3gl.active_vp = NULL;
    for (int i = 0; i < PS3GL_MAX_TMUS; i++)
        ps3gl.tmu[i].dirty = 1;
}

void ps3gl_end_frame(void)
{
    /* Must run before anything else in this function, in particular before
     * the fence-label write below: a pending deferred GL_POLYGON batch
     * (ps3gl_vertices.c) submits vertex fetches of its own, and those need
     * to be covered by this frame's fence just like every other draw --
     * exactly the hazard PS3GL_VRING_SEGMENTS' fencing exists to prevent. */
    ps3gl_flush_polygon_batch();

    /* Backend label write, after this frame's draws: RSX only writes fence_val to fence_label
     * once it has drained every preceding vertex fetch -- begin_frame seeing this value is what makes the segment safe to reuse. */
    gcmContextData *ctx = ps3gl_get_ctx();
    int seg;

    /* Frame-time telemetry: aggregate min/max/avg over a fixed window and
     * report once, rather than logging per frame (which would itself perturb
     * the timing it's measuring, same reasoning as the vring peak report
     * below). Deliberately not routed through PS3_DIAG -- that channel is a
     * budgeted, one-shot freeze-hunting tool, not meant for continuous
     * per-frame telemetry. */
    {
        static double  last_time     = 0.0;
        static double  window_start  = 0.0;
        static double  window_min    = 1e30;
        static double  window_max    = 0.0;
        static double  window_sum    = 0.0;
        static double  window_fence_sum = 0.0;
        static uint64_t window_flush_sum = 0;
        static uint64_t window_flush_vertex_sum = 0;
        static uint32_t window_count = 0;
        static uint32_t window_tex_alloc_sum = 0;
        static uint32_t window_subimage_sum = 0;
        static uint32_t window_subimage_pixels_sum = 0;
        static uint32_t window_defer_overflow_sum = 0;
        static uint32_t window_dlight_surf_sum = 0;
        static uint64_t window_dlight_luxel_sum = 0;
        static uint64_t window_batched_poly_sum = 0;
        static uint32_t window_mip_upload_sum = 0;
        static uint32_t window_mipped_bind_sum = 0;
        static uint32_t window_tex_bind_sum = 0;
        static double   window_flip_wait_sum = 0.0;
        static uint64_t window_flip_sleep_sum = 0;
        static double   window_phase_input = 0.0, window_phase_server = 0.0;
        static double   window_phase_client = 0.0, window_phase_render = 0.0;
        static double   window_phase_sound = 0.0, window_phase_total = 0.0;
        static double   window_sv_think = 0.0, window_sv_link = 0.0;
        static uint64_t window_sv_think_calls = 0, window_sv_link_calls = 0;
        static double   window_sv_readpackets = 0.0, window_sv_gameframe = 0.0;
        static double   window_sv_sendmsgs = 0.0;
        static double   window_sv_addents = 0.0, window_sv_emit = 0.0;
        static uint64_t window_sv_addents_calls = 0, window_sv_fullpack_calls = 0;
        static uint64_t window_delta_visited = 0, window_delta_written = 0;
        static uint64_t window_delta_entities = 0, window_delta_with_encoder = 0;
        static uint64_t window_bl_calls = 0, window_bl_tests = 0, window_bl_iters = 0;
        static uint64_t window_bl_newents = 0;
        static unsigned int window_bl_num_instanced = 0;
        static double   window_scene_setup = 0.0, window_scene_world = 0.0;
        static double   window_scene_ents = 0.0, window_scene_water = 0.0;
        static uint64_t window_scene_passes = 0;
        static uint64_t window_ent_studio = 0, window_ent_brush = 0;
        static uint64_t window_ent_sprite = 0, window_ent_efx = 0;
        static uint64_t window_ent_clienttri = 0, window_ent_viewmodel = 0;
        static uint64_t window_stu_client = 0, window_stu_entlight = 0;
        static uint64_t window_stu_skin = 0, window_stu_light = 0;
        static uint64_t window_stu_build = 0, window_stu_models = 0;
        static uint64_t window_stu_fill = 0, window_stu_submit = 0;
        static uint64_t window_direct_binds = 0, window_fallback_binds = 0;
        static uint64_t window_stu_cxform = 0, window_stu_cbones = 0;
        static uint64_t window_stu_cevents = 0;
        static uint64_t window_fallback_verts = 0;
        double now = Platform_DoubleTime();

        if (last_time != 0.0) {
            double dt = now - last_time;
            if (window_start == 0.0) window_start = now;
            if (dt < window_min) window_min = dt;
            if (dt > window_max) window_max = dt;
            window_sum += dt;
            window_fence_sum += ps3gl_frame_fence_wait_time;
            window_flush_sum += ps3gl_frame_flush_count;
            window_flush_vertex_sum += ps3gl_frame_flush_vertex_count;
            window_tex_alloc_sum += ps3gl_frame_tex_alloc_count;
            window_subimage_sum += ps3gl_frame_subimage_count;
            window_subimage_pixels_sum += ps3gl_frame_subimage_pixels;
            window_defer_overflow_sum += ps3gl_frame_defer_overflow_count;
            window_dlight_surf_sum += g_dlight_surface_count;
            window_dlight_luxel_sum += g_dlight_luxel_count;
            window_batched_poly_sum += ps3gl_frame_batched_poly_count;
            window_mip_upload_sum += ps3gl_frame_mip_uploads;
            window_mipped_bind_sum += ps3gl_frame_mipped_binds;
            window_tex_bind_sum += ps3gl_frame_tex_binds;
            window_flip_wait_sum += g_flip_wait_time;
            window_flip_sleep_sum += g_flip_wait_sleeps;
            window_phase_input  += g_phase_input;
            window_phase_server += g_phase_server;
            window_phase_client += g_phase_client;
            window_phase_render += g_phase_render;
            window_phase_sound  += g_phase_sound;
            window_phase_total  += g_phase_total;
            window_sv_think       += g_phase_sv_think;
            window_sv_link        += g_phase_sv_link;
            window_sv_think_calls += g_phase_sv_think_calls;
            window_sv_link_calls  += g_phase_sv_link_calls;
            window_sv_readpackets += g_phase_sv_readpackets;
            window_sv_gameframe   += g_phase_sv_gameframe;
            window_sv_sendmsgs    += g_phase_sv_sendmsgs;
            window_sv_addents     += g_phase_sv_addents;
            window_sv_emit        += g_phase_sv_emit;
            window_sv_addents_calls  += g_phase_sv_addents_calls;
            window_sv_fullpack_calls += g_phase_sv_fullpack_calls;
            window_delta_visited      += g_delta_visited;
            window_delta_written      += g_delta_written;
            window_delta_entities     += g_delta_entities;
            window_delta_with_encoder += g_delta_with_encoder;
            window_bl_calls   += g_bl_calls;
            window_bl_tests   += g_bl_tests;
            window_bl_iters   += g_bl_iters;
            window_bl_newents += g_bl_newents;
            window_bl_num_instanced = g_bl_num_instanced; /* state, not a sum */
            window_scene_setup  += g_scene_setup;
            window_scene_world  += g_scene_world;
            window_scene_ents   += g_scene_ents;
            window_scene_water  += g_scene_water;
            window_scene_passes += g_scene_passes;
            window_ent_studio    += g_ent_studio;
            window_ent_brush     += g_ent_brush;
            window_ent_sprite    += g_ent_sprite;
            window_ent_efx       += g_ent_efx;
            window_ent_clienttri += g_ent_clienttri;
            window_ent_viewmodel += g_ent_viewmodel;
            window_stu_client    += g_stu_client;
            window_stu_entlight  += g_stu_entlight;
            window_stu_skin      += g_stu_skin;
            window_stu_light     += g_stu_light;
            window_stu_build     += g_stu_build;
            window_stu_fill      += g_stu_fill;
            window_stu_submit    += g_stu_submit;
            window_stu_models    += g_stu_models;
            window_stu_cxform     += g_stu_cxform;
            window_stu_cbones     += g_stu_cbones;
            window_stu_cevents    += g_stu_cevents;
            window_direct_binds   += ps3gl_frame_direct_binds;
            window_fallback_binds += ps3gl_frame_fallback_binds;
            window_fallback_verts += ps3gl_frame_fallback_verts;
            window_count++;

            if (now - window_start >= 5.0 && window_count > 0) {
                double avg = window_sum / window_count;
                double avg_fence = window_fence_sum / window_count;
                double avg_flushes = (double)window_flush_sum / window_count;
                double avg_verts_per_flush = window_flush_sum > 0 ?
                    (double)window_flush_vertex_sum / (double)window_flush_sum : 0.0;
                ps3gl_log("[ps3gl] frame time: avg %.2fms (%.1f fps) min %.2fms max %.2fms "
                          "over %u frames\n",
                          avg * 1000.0, avg > 0.0 ? 1.0 / avg : 0.0,
                          window_min * 1000.0, window_max * 1000.0,
                          (unsigned)window_count);
                ps3gl_log("[ps3gl] frame breakdown: vring fence wait avg %.2fms (%.0f%% of frame), "
                          "avg %.1f draw flushes/frame, avg %.1f verts/flush\n",
                          avg_fence * 1000.0, avg > 0.0 ? (avg_fence / avg) * 100.0 : 0.0,
                          avg_flushes, avg_verts_per_flush);
                {
                    /* THE decisive CPU-vs-GPU split (see ps3gl_report_flip_wait).
                     * High % here = GPU-bound. ~0 = CPU-bound, and no amount of
                     * renderer work will help. us/sleep far above 100 = LV2
                     * usleep granularity, a third and different problem. */
                    double avg_flip = window_flip_wait_sum / window_count;
                    double avg_sleeps = (double)window_flip_sleep_sum / window_count;
                    ps3gl_log("[ps3gl] flip wait: avg %.2fms (%.0f%% of frame), "
                              "avg %.1f sleeps/frame (%.0f us/sleep)\n",
                              avg_flip * 1000.0,
                              avg > 0.0 ? (avg_flip / avg) * 100.0 : 0.0,
                              avg_sleeps,
                              window_flip_sleep_sum > 0 ?
                                  (window_flip_wait_sum * 1e6) / (double)window_flip_sleep_sum : 0.0);
                }
                {
                    /* The whole point of this line: `render` here EXCLUDES the
                     * flip wait reported above (subtracted), so it is real
                     * renderer CPU. Whichever phase dominates is where the
                     * ~50fps floor actually lives -- and `other` catching it
                     * would mean the cost is outside all four (HTTP_Run,
                     * XRcon_Frame, Host_ClientBegin, Host_GetCommands). */
                    double n = (double)window_count;
                    double render_cpu = (window_phase_render - window_flip_wait_sum) / n;
                    double other = (window_phase_total - window_phase_input - window_phase_server
                                    - window_phase_client - window_phase_render
                                    - window_phase_sound) / n;
                    ps3gl_log("[ps3gl] cpu phases: input %.2f server %.2f client %.2f "
                              "render %.2f (minus flip wait) sound %.2f other %.2f "
                              "| host total %.2fms\n",
                              (window_phase_input / n) * 1000.0,
                              (window_phase_server / n) * 1000.0,
                              (window_phase_client / n) * 1000.0,
                              render_cpu * 1000.0,
                              (window_phase_sound / n) * 1000.0,
                              other * 1000.0,
                              (window_phase_total / n) * 1000.0);
                }
                {
                    /* Splits the `server` figure above. `think` is the game
                     * DLL's per-entity think (an env_rope's whole RopeThink --
                     * rope sim AND its per-segment traces -- is in here);
                     * `link` is SV_LinkEdict's areanode relink, which every
                     * MOVETYPE_NOCLIP entity pays once per frame (a rope
                     * spawns 2 segment entities per segment, all NOCLIP).
                     * `rest` is server minus both: packets, client commands.
                     * Call counts distinguish "expensive per call" from
                     * "called an enormous number of times". */
                    double n = (double)window_count;
                    double sv_think = window_sv_think / n;
                    double sv_link  = window_sv_link / n;
                    double sv_rest  = (window_phase_server / n) - sv_think - sv_link;
                    ps3gl_log("[ps3gl] server phases: think %.2f (%.0f calls) "
                              "link %.2f (%.0f calls) rest %.2f ms | server %.2fms\n",
                              sv_think * 1000.0,
                              (double)window_sv_think_calls / n,
                              sv_link * 1000.0,
                              (double)window_sv_link_calls / n,
                              sv_rest * 1000.0,
                              (window_phase_server / n) * 1000.0);
                }
                {
                    /* Splits `server` a second way -- by the three top-level
                     * calls in Host_ServerFrame instead of by think/link.
                     * gameframe CONTAINS the think and link figures above
                     * (SV_Physics nests them), so gameframe minus think minus
                     * link is the physics loop's own per-entity overhead.
                     * sendmsgs is snapshot build + delta encoding, which
                     * scales with networked entity count. `other` is the
                     * remaining bookkeeping (movevars, timeouts, PrepWorldFrame). */
                    double n = (double)window_count;
                    double rp = window_sv_readpackets / n;
                    double gf = window_sv_gameframe / n;
                    double sm = window_sv_sendmsgs / n;
                    double oth = (window_phase_server / n) - rp - gf - sm;
                    ps3gl_log("[ps3gl] server split: readpackets %.2f gameframe %.2f "
                              "sendmsgs %.2f other %.2f ms (gameframe incl think+link)\n",
                              rp * 1000.0, gf * 1000.0, sm * 1000.0, oth * 1000.0);
                }
                {
                    /* Splits `sendmsgs`. `addents` is the per-entity
                     * visibility walk (SV_AddEntitiesToPacket -> the game
                     * DLL's pfnAddToFullPack, once per entity per client);
                     * `emit` is the delta encode (MSG_WriteDeltaEntity).
                     * fullpack/frame is the entity count that walk visits --
                     * if the rope segments dominate, this is where they show
                     * up, and cost/call separates "too many entities" from
                     * "each entity too expensive". */
                    double n = (double)window_count;
                    double ae = window_sv_addents / n;
                    double em = window_sv_emit / n;
                    double fp = (double)window_sv_fullpack_calls / n;
                    ps3gl_log("[ps3gl] snapshot split: addents %.2f emit %.2f ms "
                              "| %.0f fullpack calls/frame (%.1f us/call) "
                              "| %.1f snapshots/frame\n",
                              ae * 1000.0, em * 1000.0, fp,
                              fp > 0.0 ? (ae / fp) * 1000000.0 : 0.0,
                              (double)window_sv_addents_calls / n);
                }
                {
                    /* Explains emit's per-entity cost. written/visited near
                     * 1.00 together with encoders 0 means the game DLL's
                     * delta encoders were wiped, field suppression is off,
                     * and every field is transmitted unconditionally -- a
                     * known regression on this port. A small ratio means
                     * suppression works and the comparison scan is the cost. */
                    double n = (double)window_count;
                    double vis = (double)window_delta_visited;
                    double wr  = (double)window_delta_written;
                    ps3gl_log("[ps3gl] delta fields: %.0f visited %.0f written "
                              "(%.2f written/visited) | %.0f ents/frame "
                              "%.0f w/encoder | %.1f fields/ent\n",
                              vis / n, wr / n,
                              vis > 0.0 ? wr / vis : 0.0,
                              (double)window_delta_entities / n,
                              (double)window_delta_with_encoder / n,
                              window_delta_entities > 0
                                  ? vis / (double)window_delta_entities : 0.0);
                }
                {
                    /* The work the delta-field counters above cannot see.
                     * Each `test` is a full Delta_TestBaseline field loop, so
                     * est.compares = tests x fields/ent should reconcile with
                     * emit's measured time. num_instanced 0 means the cheap
                     * classname path was never available and every new entity
                     * paid the full backward search. */
                    double n = (double)window_count;
                    double fpe = window_delta_entities > 0
                        ? (double)window_delta_visited / (double)window_delta_entities : 0.0;
                    ps3gl_log("[ps3gl] baseline search: %.1f calls %.1f tests %.1f iters "
                              "%.1f newents/frame | num_instanced %u | est. %.0f field compares/frame\n",
                              (double)window_bl_calls / n,
                              (double)window_bl_tests / n,
                              (double)window_bl_iters / n,
                              (double)window_bl_newents / n,
                              window_bl_num_instanced,
                              ((double)window_bl_tests / n) * fpe);
                }
                {
                    /* Sums to the `render` figure above minus 2D/HUD drawing,
                     * so a large gap between (setup+world+ents+water) and
                     * `render` means the cost is in the HUD/console/menu pass,
                     * not the 3D scene -- a bucket nothing else covers.
                     * `passes` > 1 means a mirror/portal/water-reflection pass
                     * is re-running the whole scene. */
                    double n = (double)window_count;
                    ps3gl_log("[ps3gl] scene phases: setup %.2f world %.2f entities %.2f "
                              "water %.2f | %.1f passes/frame\n",
                              (window_scene_setup / n) * 1000.0,
                              (window_scene_world / n) * 1000.0,
                              (window_scene_ents / n) * 1000.0,
                              (window_scene_water / n) * 1000.0,
                              (double)window_scene_passes / n);
                }
                {
                    /* Splits the `entities` figure above. The previous session
                     * assumed that phase was all studio models; it is not --
                     * everything on this line lives inside R_DrawEntitiesOnList.
                     * A large `clienttri` means the cost is in the game DLL's
                     * HUD/tri callbacks, which no renderer change can touch. */
                    double n = (double)window_count;
                    ps3gl_log("[ps3gl] entity phases: studio %.2f brush %.2f sprite %.2f "
                              "efx %.2f clienttri %.2f viewmodel %.2f ms\n",
                              ps3gl_ticks_to_sec(window_ent_studio) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_ent_brush) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_ent_sprite) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_ent_efx) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_ent_clienttri) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_ent_viewmodel) / n * 1000.0);
                }
                {
                    /* Splits `studio` + `viewmodel` above, and now fully
                     * accounts for them: client+entlight+skin+light+build should
                     * reconcile with those two (`client` is derived by
                     * subtraction, so it absorbs any residual by construction).
                     * fill/submit are sub-items OF build -- build minus both is
                     * the per-mesh GL state churn. Round 1 measured build 4.82ms
                     * vs light 1.58, so the target is vertex packing and draw
                     * submission, NOT per-normal lighting. */
                    double n = (double)window_count;
                    ps3gl_log("[ps3gl] studio phases: client %.2f entlight %.2f skin %.2f "
                              "light %.2f build %.2f (fill %.2f submit %.2f) ms "
                              "| %.1f models/frame\n",
                              ps3gl_ticks_to_sec(window_stu_client) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_entlight) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_skin) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_light) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_build) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_fill) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_submit) / n * 1000.0,
                              (double)window_stu_models / n);
                    /* Sub-items of `client`, measured inside the game DLL's own
                     * studio renderer -- the only place that code is visible. */
                    ps3gl_log("[ps3gl] studio client: xform %.2f bones %.2f events %.2f ms "
                              "(sub-items of `client` above)\n",
                              ps3gl_ticks_to_sec(window_stu_cxform) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_cbones) / n * 1000.0,
                              ps3gl_ticks_to_sec(window_stu_cevents) / n * 1000.0);
                }
                {
                    /* The premise check for the whole host-ring change: if
                     * fallback is ~0 then studio draws were already direct-bound
                     * and `submit` is something else. hring drops mean the ring
                     * is undersized and those runs silently fell back to BSS. */
                    double n = (double)window_count;
                    ps3gl_log("[ps3gl] drawelements: %.1f direct binds/frame, %.1f fallback "
                              "binds/frame (%.0f verts copied/frame) | hring peak %u KB "
                              "of %u KB, %u drops\n",
                              (double)window_direct_binds / n,
                              (double)window_fallback_binds / n,
                              (double)window_fallback_verts / n,
                              (unsigned)(ps3gl.hring.peak_head / 1024u),
                              (unsigned)(ps3gl.hring.seg_capacity / 1024u),
                              (unsigned)ps3gl.hring.total_drops);
                }
                ps3gl_log("[ps3gl] texture uploads: %u alloc (new/resized), %u subimage "
                          "(%u px converted), %u deferred-free overflow flushes\n",
                          (unsigned)window_tex_alloc_sum, (unsigned)window_subimage_sum,
                          (unsigned)window_subimage_pixels_sum, (unsigned)window_defer_overflow_sum);
                {
                    double avg_dlight_surf = (double)window_dlight_surf_sum / window_count;
                    double avg_luxel_per_surf = window_dlight_surf_sum > 0 ?
                        (double)window_dlight_luxel_sum / (double)window_dlight_surf_sum : 0.0;
                    ps3gl_log("[ps3gl] dlight breakdown: avg %.1f dlit surfaces/frame, "
                              "%.0f total luxels (avg %.1f luxels/surf)\n",
                              avg_dlight_surf, (double)window_dlight_luxel_sum,
                              avg_luxel_per_surf);
                }
                ps3gl_log("[ps3gl] polygon batching: avg %.1f GL_POLYGON draws merged/frame "
                          "(0 means the state-clean gate never passed this window)\n",
                          (double)window_batched_poly_sum / window_count);
                ps3gl_log("[ps3gl] mip uploads: %u level>0 uploads this window, "
                          "%u/%u binds with maxlod>0 (0 mipped => num_levels never grew)\n",
                          (unsigned)window_mip_upload_sum, (unsigned)window_mipped_bind_sum,
                          (unsigned)window_tex_bind_sum);
                {
                    /* Once per 5s window, not per frame: cheap scan (<=4096
                     * slots) over the whole texture pool answering "does the
                     * mip pyramid actually exist in VRAM right now", the
                     * strongest CPU-side evidence short of the debug-tint
                     * visual test. */
                    unsigned live = 0, mipped = 0, maxlv = 0, swizzled = 0;
                    uint64_t lvsum = 0;
                    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
                        const ps3gl_texture_t *t = &ps3gl.textures[i];
                        if (t->glname == -1 || !t->data) continue;
                        live++;
                        if (t->swizzled) swizzled++;
                        if (t->num_levels > 1) {
                            mipped++;
                            lvsum += t->num_levels;
                            if (t->num_levels > maxlv) maxlv = t->num_levels;
                        }
                    }
                    /* swizzled count is not optional instrumentation: with the
                     * layout change measuring flat against linear, "it made no
                     * difference" and "it never engaged" produce identical logs
                     * AND identical visuals without this number. */
                    ps3gl_log("[ps3gl] texture layout: %u/%u live textures swizzled, "
                              "%u mipped (avg %.1f levels, max %u)\n",
                              swizzled, live, mipped,
                              mipped > 0 ? (double)lvsum / mipped : 0.0, maxlv);
                }
                window_start = now;
                window_min   = 1e30;
                window_max   = 0.0;
                window_sum   = 0.0;
                window_fence_sum = 0.0;
                window_flush_sum = 0;
                window_flush_vertex_sum = 0;
                window_count = 0;
                window_tex_alloc_sum = 0;
                window_subimage_sum = 0;
                window_subimage_pixels_sum = 0;
                window_defer_overflow_sum = 0;
                window_dlight_surf_sum = 0;
                window_dlight_luxel_sum = 0;
                window_batched_poly_sum = 0;
                window_mip_upload_sum = 0;
                window_mipped_bind_sum = 0;
                window_tex_bind_sum = 0;
                window_flip_wait_sum = 0.0;
                window_flip_sleep_sum = 0;
                window_phase_input = window_phase_server = window_phase_client = 0.0;
                window_phase_render = window_phase_sound = window_phase_total = 0.0;
                window_sv_think = window_sv_link = 0.0;
                window_sv_think_calls = window_sv_link_calls = 0;
                window_sv_readpackets = window_sv_gameframe = 0.0;
                window_sv_sendmsgs = 0.0;
                window_sv_addents = window_sv_emit = 0.0;
                window_sv_addents_calls = window_sv_fullpack_calls = 0;
                window_delta_visited = window_delta_written = 0;
                window_delta_entities = window_delta_with_encoder = 0;
                window_bl_calls = window_bl_tests = window_bl_iters = 0;
                window_bl_newents = 0;
                window_scene_setup = window_scene_world = 0.0;
                window_scene_ents = window_scene_water = 0.0;
                window_scene_passes = 0;
                window_ent_studio = window_ent_brush = window_ent_sprite = 0;
                window_ent_efx = window_ent_clienttri = window_ent_viewmodel = 0;
                window_stu_client = window_stu_entlight = window_stu_skin = 0;
                window_stu_light = window_stu_build = window_stu_models = 0;
                window_stu_fill = window_stu_submit = 0;
                window_direct_binds = window_fallback_binds = 0;
                window_stu_cxform = window_stu_cbones = window_stu_cevents = 0;
                window_fallback_verts = 0;
            }
        }
        last_time = now;
        ps3gl_frame_fence_wait_time = 0.0;
        ps3gl_frame_flush_count = 0;
        ps3gl_frame_flush_vertex_count = 0;
        ps3gl_frame_tex_alloc_count = 0;
        ps3gl_frame_subimage_count = 0;
        ps3gl_frame_subimage_pixels = 0;
        ps3gl_frame_defer_overflow_count = 0;
        ps3gl_frame_batched_poly_count = 0;
        ps3gl_frame_mip_uploads = 0;
        ps3gl_frame_mipped_binds = 0;
        ps3gl_frame_tex_binds = 0;
        g_flip_wait_time = 0.0;
        g_flip_wait_sleeps = 0;
        g_scene_setup = g_scene_world = g_scene_ents = g_scene_water = 0.0;
        g_scene_passes = 0;
        g_ent_studio = g_ent_brush = g_ent_sprite = 0;
        g_ent_efx = g_ent_clienttri = g_ent_viewmodel = 0;
        g_stu_client = g_stu_entlight = g_stu_skin = 0;
        g_stu_light = g_stu_build = g_stu_models = 0;
        g_stu_fill = g_stu_submit = 0;
        g_stu_cxform = g_stu_cbones = g_stu_cevents = 0;
        ps3gl_frame_direct_binds = 0;
        ps3gl_frame_fallback_binds = 0;
        ps3gl_frame_fallback_verts = 0;
    }

    /* Report vertex-ring exhaustion once per affected frame. Dropped draws mean
     * geometry is missing from what the player sees, so this is never noise --
     * if it appears at all, PS3GL_VRING_SIZE is too small. peak_head is the
     * high-water mark to size it from. */
    if (ps3gl.vring.frame_drops) {
        ps3gl_log("[ps3gl] WARNING: vertex ring exhausted -- dropped %u draw(s) this "
                  "frame (%u total). peak=%u of %u per segment; raise PS3GL_VRING_SIZE\n",
                  (unsigned)ps3gl.vring.frame_drops, (unsigned)ps3gl.vring.total_drops,
                  (unsigned)ps3gl.vring.peak_head, (unsigned)ps3gl.vring.seg_capacity);
        ps3gl.vring.frame_drops = 0;
    }

    /* Report the high-water mark as it grows, so the ring can be sized from a
     * real play session rather than a guess. Only on a 256 KB step, so a long
     * session produces a handful of lines, not one per frame. */
    {
        static uint32_t last_reported_peak = 0;
        if (ps3gl.vring.peak_head >= last_reported_peak + (256u * 1024u)) {
            last_reported_peak = ps3gl.vring.peak_head;
            ps3gl_log("[ps3gl] vertex ring peak: %u KB of %u KB per segment\n",
                      (unsigned)(ps3gl.vring.peak_head / 1024u),
                      (unsigned)(ps3gl.vring.seg_capacity / 1024u));
        }
    }

    if (!ctx) return;
    seg = ps3gl.vring.cur_seg;
    if (ctx && ps3gl.vring.fence_label[seg]) {
        ps3gl.vring.fence_val[seg]++;
        rsxSetWriteBackendLabel(ctx,
            seg == 0 ? PS3GL_LABEL_VRING_SEG0 : PS3GL_LABEL_VRING_SEG1,
            ps3gl.vring.fence_val[seg]);
    }
}
