/* ps3gl_shaders.c -- GL-to-RSX layer: shader program management. One shared vertex
 * program, one fragment program per texenv mode. If PS3GL_SHADERS_AVAILABLE==0 the shaders weren't compiled -- every draw is a silent no-op, nothing renders. */

#include "ps3gl.h"
#include "ps3gl_shader_data.h"
#include <stdio.h>

/* Forward declarations */

#if PS3GL_SHADERS_AVAILABLE

/* Shader loading helpers */

static void load_vp(ps3gl_shader_t *s, const void *data, uint32_t size)
{
    s->vp = (rsxVertexProgram *)data;
    rsxVertexProgramGetUCode(s->vp, &s->vp_ucode, &s->vp_ucode_size);
}

static void load_fp(ps3gl_shader_t *s, const void *data, uint32_t size)
{
    s->fp = (rsxFragmentProgram *)data;

    void *ucode;
    rsxFragmentProgramGetUCode(s->fp, &ucode, &s->fp_ucode_size);

    /* Fragment program ucode must reside in RSX-accessible memory */
    s->fp_ucode = rsxMemalign(64, s->fp_ucode_size);
    if (s->fp_ucode) {
        memcpy(s->fp_ucode, ucode, s->fp_ucode_size);
        rsxAddressToOffset(s->fp_ucode, &s->fp_offset);
    }
}

void ps3gl_shaders_init(void)
{
    ps3gl_log("[ps3gl] Loading compiled shaders...\n");

    /* All shader slots share the same vertex program */
    for (int i = 0; i < PS3GL_TENV_COUNT; i++) {
        memset(&ps3gl.shaders[i], 0, sizeof(ps3gl_shader_t));
        load_vp(&ps3gl.shaders[i], shader_vp_data, shader_vp_data_size);
        ps3gl.shaders[i].mvp_const = rsxVertexProgramGetConst(ps3gl.shaders[i].vp, "mvp");
    }

    /* Load per-mode fragment programs */
    load_fp(&ps3gl.shaders[PS3GL_TENV_DISABLED], shader_fp_coloronly_data, shader_fp_coloronly_data_size);
    load_fp(&ps3gl.shaders[PS3GL_TENV_MODULATE], shader_fp_modulate_data, shader_fp_modulate_data_size);
    load_fp(&ps3gl.shaders[PS3GL_TENV_REPLACE],  shader_fp_replace_data,  shader_fp_replace_data_size);
    load_fp(&ps3gl.shaders[PS3GL_TENV_DECAL],    shader_fp_decal_data,    shader_fp_decal_data_size);
    load_fp(&ps3gl.shaders[PS3GL_TENV_ADD],       shader_fp_add_data,      shader_fp_add_data_size);
    load_fp(&ps3gl.shaders[PS3GL_TENV_BLEND],     shader_fp_blend_data,    shader_fp_blend_data_size);
    load_fp(&ps3gl.shaders[PS3GL_TENV_MODULATE2], shader_fp_modulate2_data, shader_fp_modulate2_data_size);

    ps3gl.active_shader = -1;
    ps3gl.active_vp = NULL;
    ps3gl.mvp_uploaded_gen = 0;
    ps3gl_log("[ps3gl] Shaders loaded: %d modes\n", PS3GL_TENV_COUNT);
}

void ps3gl_shaders_shutdown(void)
{
    for (int i = 0; i < PS3GL_TENV_COUNT; i++) {
        if (ps3gl.shaders[i].fp_ucode) {
            rsxFree(ps3gl.shaders[i].fp_ucode);
            ps3gl.shaders[i].fp_ucode = NULL;
        }
    }
}

int ps3gl_shader_key(void)
{
    int tmu0 = ps3gl.tmu[0].enabled && ps3gl.tmu[0].bound && ps3gl.tmu[0].bound->data;
    int tmu1 = ps3gl.tmu[1].enabled && ps3gl.tmu[1].bound && ps3gl.tmu[1].bound->data;

    /* Both TMUs active: use dual-texture modulate (diffuse * lightmap). */
    if (tmu0 && tmu1)
        return PS3GL_TENV_MODULATE2;

    if (tmu0)
        return ps3gl.tmu[0].texenv;

    return PS3GL_TENV_DISABLED;
}

void ps3gl_apply_shader(void)
{
    int key = ps3gl_shader_key();
    if (key < 0 || key >= PS3GL_TENV_COUNT) key = PS3GL_TENV_DISABLED;

    ps3gl_shader_t *s = &ps3gl.shaders[key];
    gcmContextData *ctx = ps3gl_get_ctx();
    if (!ctx) return;

    /* All shader slots share one VP (see ps3gl_shaders_init) -- reload it only
     * when it's not already bound, not on every FP/texenv key change. */
    int vp_reloaded = 0;
    if (s->vp != ps3gl.active_vp) {
        rsxLoadVertexProgram(ctx, s->vp, s->vp_ucode);
        ps3gl.active_vp = s->vp;
        vp_reloaded = 1;
    }

    if (key != ps3gl.active_shader) {
        if (s->fp_ucode) {
            rsxLoadFragmentProgramLocation(ctx, s->fp, s->fp_offset,
                                           GCM_LOCATION_RSX);
        }

        ps3gl.active_shader = key;
    }

    /* MVP constant is independent of which FP is bound -- only re-patch it
     * when it changed since the last upload (patching RSX microcode isn't free). */
    uint32_t mvp_gen = ps3gl_get_mvp_generation();
    if (s->mvp_const && mvp_gen != ps3gl.mvp_uploaded_gen) {
        const float *mvp = ps3gl_get_mvp();
        rsxSetVertexProgramParameter(ctx, s->vp, s->mvp_const, mvp);
        ps3gl.mvp_uploaded_gen = mvp_gen;
    }

    /* Upload the world-space clip plane only if this shader binary was recompiled to support
     * it (clip_plane_const is NULL on the old pre-clip-plane binary -- software clip in ps3gl_draw.c covers that case).
     *
     * Guarded on the value actually changing, like the MVP above: this runs on
     * every state-applying flush, and patching vertex-program constants is not
     * free. The plane only moves when a portal/mirror pass sets it, i.e. a
     * handful of times per frame at most, not once per draw. */
    if (s->clip_plane_const &&
        (vp_reloaded ||
         memcmp(ps3gl.clip_plane_uploaded, ps3gl.clip_plane, sizeof(ps3gl.clip_plane)) != 0)) {
        rsxSetVertexProgramParameter(ctx, s->vp, s->clip_plane_const,
                                     ps3gl.clip_plane);
        memcpy(ps3gl.clip_plane_uploaded, ps3gl.clip_plane, sizeof(ps3gl.clip_plane));
    }
}

#else /* PS3GL_SHADERS_AVAILABLE == 0 */

/* Stub implementations when shaders are not compiled */

void ps3gl_shaders_init(void)
{
    ps3gl_log("[ps3gl] WARNING: No compiled shaders available.\n");
    ps3gl_log("[ps3gl] Build ps3toolchain, then run: cd 3rdparty/ps3gl/shaders && ./compile_shaders.sh\n");
    ps3gl.active_shader = -1;
}

void ps3gl_shaders_shutdown(void) {}

int ps3gl_shader_key(void) { return PS3GL_TENV_DISABLED; }

void ps3gl_apply_shader(void)
{
    /* No shaders to apply -- draws will produce no visible output */
}

#endif /* PS3GL_SHADERS_AVAILABLE */
