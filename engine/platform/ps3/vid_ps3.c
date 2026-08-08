/*
vid_ps3.c - PS3 (PSL1GHT/RSX) video backend
Copyright (C) 2026 xashPS3 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "platform/platform.h"
#if XASH_VIDEO == VIDEO_PS3
#include "input.h"
#include "client.h"
#include "filesystem.h"
#include "vid_common.h"
#include "ps3_diag.h"
#include <stdlib.h>
#include <string.h> // memcpy
#include <malloc.h> // memalign -- not declared by this libc's stdlib.h
#include <unistd.h> // usleep
// Same -Werror=strict-prototypes fixup as in_ps3.c/sys_ps3.c's io/pad.h and net/net.h wraps.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#pragma GCC diagnostic pop
#include <sysutil/video.h>
#include "ps3gl.h" // 3rdparty/ps3gl -- RSX-backed GL 1.1, linked into the engine

// Two renderers share this backend: REF_SOFTWARE draws into ps3_swbuffer (XDR)
// then SW_UnlockBuffer copies it into the RSX display buffer (presentation-only,
// no render target/depth). REF_GL issues real GL calls via ps3gl straight into
// the display buffer, needing a Z24S8 depth buffer and an rsxSetSurface target.
// Never read the RSX/GDDR3 buffer back on the PPE (~16 MB/s) in either mode.

#define PS3_SCREEN_WIDTH  1280
#define PS3_SCREEN_HEIGHT 720

// Values match tools/ps3_video, hardware-validated -- rsx.h's own doc-comment
// defaults (64KB CB / 1MB host) failed on real hardware.
#define PS3_RSX_CB_SIZE    ( 1 * 1024 * 1024 )
#define PS3_RSX_HOST_SIZE  ( 32 * 1024 * 1024 )
// 3, not 2: with only 2 buffers, any frame exceeding one vsync interval gets
// bumped a full extra tick, quantizing to a hard 30fps under GCM_FLIP_VSYNC.
// Matches the sibling ioQuake3-PS3 port's RSX_FB_COUNT.
#define PS3_FB_COUNT       3

static byte *ps3_swbuffer;

static gcmContextData *ps3_gcm_context;
static void   *ps3_rsx_host_addr;
static u32     ps3_color_pitch;
static u32     ps3_color_offset[PS3_FB_COUNT];
static void   *ps3_color_buffer[PS3_FB_COUNT];
static u32     ps3_display_width = PS3_SCREEN_WIDTH;
static u32     ps3_display_height = PS3_SCREEN_HEIGHT;
static int     ps3_current_fb;
static qboolean ps3_rsx_ready;

// REF_GL only. -1 means "nothing bound yet", so the first SetRenderTarget(0)
// isn't swallowed by its own no-change early-out.
static void   *ps3_depth_buffer;
static u32     ps3_depth_offset;
static int     ps3_current_rt = -1;
static qboolean ps3_gl_mode;

// Flip completion via gcmSetFlipHandler, not gcmGetFlipStatus() polling --
// polling read back "already completed" instantly on real hardware.
static volatile u32 ps3_flip_queued;
static volatile u32 ps3_flip_completed;

static void PS3_RSX_FlipHandler( const u32 head )
{
	(void)head;
	ps3_flip_completed++;
}

// Block until the buffer we're about to overwrite is free (not still being scanned out).
static void PS3_RSX_WaitForFreeBuffer( void )
{
	int waited = 0;
	// The only place a GPU-bound CPU actually blocks; timed here so frame time is
	// fully accounted (see ps3gl_report_flip_wait) instead of misreading 0% as "no GPU wait".
	double wait_start = Platform_DoubleTime();

	while( (int)( ps3_flip_queued - ps3_flip_completed ) > PS3_FB_COUNT - 2 )
	{
		usleep( 100 );
		if( ++waited > 20000 )
		{
			// Force-resync, not just break: else flip_completed stays behind flip_queued
			// forever, and every later call fires early.
			Con_Printf( "PS3_RSX_WaitForFreeBuffer: flip fence timed out, force-syncing\n" );
			ps3_flip_completed = ps3_flip_queued;
			break;
		}
	}

	// Only meaningful in GL mode -- ref_soft has no ps3gl telemetry window.
	if( ps3_gl_mode )
		ps3gl_report_flip_wait( Platform_DoubleTime() - wait_start, (uint32_t)waited );
}

// REF_GL only. Allocated lazily from R_Init_Video( REF_GL ) so the ref_soft
// path never pays for a depth buffer it can't use (~8MB of GDDR3 at 1080p).
static qboolean PS3_RSX_AllocDepthBuffer( void )
{
	if( ps3_depth_buffer )
		return true;

	ps3_depth_buffer = rsxMemalign( 64, ps3_display_width * ps3_display_height * 4 );
	if( !ps3_depth_buffer )
	{
		Con_Printf( "PS3_RSX_AllocDepthBuffer: rsxMemalign FAILED (%ux%u Z24S8)\n",
			ps3_display_width, ps3_display_height );
		return false;
	}

	rsxAddressToOffset( ps3_depth_buffer, &ps3_depth_offset );
	Con_Printf( "PS3_RSX_AllocDepthBuffer: %ux%u Z24S8 offset=0x%x\n",
		ps3_display_width, ps3_display_height, (uint)ps3_depth_offset );
	return true;
}

// REF_GL only. Points the RSX at the framebuffer ref_gl is about to draw into.
static void PS3_RSX_SetRenderTarget( int index )
{
	gcmSurface sf;
	int i;

	if( index == ps3_current_rt )
		return;
	ps3_current_rt = index;

	memset( &sf, 0, sizeof( sf ));

	sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = ps3_color_offset[index];
	sf.colorPitch[0]    = ps3_color_pitch;

	// MRT slots 1-3 must be populated even though only slot 0 renders -- rsxSetSurface
	// returns void, so a bad config just produces a black screen (confirmed on HW).
	for( i = 1; i < 4; i++ )
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i]   = ps3_color_offset[index];
		sf.colorPitch[i]    = 64;
	}

	sf.depthFormat   = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset   = ps3_depth_offset;
	sf.depthPitch    = ps3_display_width * 4;

	sf.type      = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;

	sf.width  = ps3_display_width;
	sf.height = ps3_display_height;
	sf.x      = 0;
	sf.y      = 0;

	rsxSetSurface( ps3_gcm_context, &sf );
}

// Exposes the current color buffer for the one-shot savegame/levelshot CPU
// readback in ps3gl_vertices.c's glReadPixels. Flushes/waits first so the copy
// sees the just-drawn frame; only acceptable once per screenshot, never per-frame.
void *PS3_RSX_GetColorBuffer( u32 *pitch, u32 *width, u32 *height )
{
	if( !ps3_rsx_ready )
		return NULL;

	rsxFlushBuffer( ps3_gcm_context );
	rsxFinish( ps3_gcm_context, 1 );

	if( pitch )  *pitch  = ps3_color_pitch;
	if( width )  *width  = ps3_display_width;
	if( height ) *height = ps3_display_height;

	return ps3_color_buffer[ps3_current_fb];
}

static qboolean PS3_RSX_Init( void )
{
	videoState state;
	videoResolution res;
	videoConfiguration vconfig;
	s32 ret;
	int i;

	if( ps3_rsx_ready )
		return true;

	ps3_rsx_host_addr = memalign( 1024 * 1024, PS3_RSX_HOST_SIZE );
	if( !ps3_rsx_host_addr )
	{
		Con_Printf( "PS3_RSX_Init: host memalign FAILED\n" );
		return false;
	}

	ret = rsxInit( &ps3_gcm_context, PS3_RSX_CB_SIZE, PS3_RSX_HOST_SIZE, ps3_rsx_host_addr );
	Con_Printf( "PS3_RSX_Init: rsxInit ret=%d context=%p\n", (int)ret, (void *)ps3_gcm_context );
	if( ret != 0 || !ps3_gcm_context )
		return false;

	// Request 720p regardless of the TV's negotiated mode -- videoConfigure genuinely
	// renegotiates the physical signal, and HW telemetry shows the renderer is
	// fill-rate-bound at 1080p (~50-58fps) but locked 60fps at 720p. Falls back to
	// the TV's actual mode only if 720p genuinely isn't offered.
	s32 vid_res = VIDEO_RESOLUTION_720;
	u8 vid_aspect = VIDEO_ASPECT_16_9;

	if( !videoGetResolutionAvailability( VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9, 0 ))
	{
		if( videoGetState( 0, 0, &state ) != 0 )
		{
			Con_Printf( "PS3_RSX_Init: videoGetState FAILED\n" );
			return false;
		}
		vid_res = state.displayMode.resolution;
		vid_aspect = state.displayMode.aspect;
		Con_Printf( "PS3_RSX_Init: 720p not available, using TV default (resolution_id=%u)\n", (uint)vid_res );
	}

	if( videoGetResolution( vid_res, &res ) != 0 )
	{
		Con_Printf( "PS3_RSX_Init: videoGetResolution FAILED\n" );
		return false;
	}

	ps3_display_width = res.width;
	ps3_display_height = res.height;
	Con_Printf( "PS3_RSX_Init: display %ux%u resolution_id=%u aspect=%u\n",
		ps3_display_width, ps3_display_height,
		(uint)vid_res, (uint)vid_aspect );

	memset( &vconfig, 0, sizeof( vconfig ));
	vconfig.resolution = vid_res;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = ps3_display_width * 4;
	vconfig.aspect = vid_aspect;
	ret = videoConfigure( 0, &vconfig, NULL, 0 );
	Con_Printf( "PS3_RSX_Init: videoConfigure ret=%d pitch=%u\n", (int)ret, (uint)vconfig.pitch );

	ps3_color_pitch = ps3_display_width * 4;

	for( i = 0; i < PS3_FB_COUNT; i++ )
	{
		ps3_color_buffer[i] = rsxMemalign( 64, ps3_color_pitch * ps3_display_height );
		if( !ps3_color_buffer[i] )
		{
			Con_Printf( "PS3_RSX_Init: rsxMemalign color[%d] FAILED\n", i );
			return false;
		}

		rsxAddressToOffset( ps3_color_buffer[i], &ps3_color_offset[i] );
		ret = gcmSetDisplayBuffer( i, ps3_color_offset[i], ps3_color_pitch, ps3_display_width, ps3_display_height );
		Con_Printf( "PS3_RSX_Init: gcmSetDisplayBuffer[%d] ret=%d offset=0x%x\n",
			i, (int)ret, (uint)ps3_color_offset[i] );
	}

	gcmSetFlipMode( GCM_FLIP_VSYNC );

	ps3_flip_queued = 0;
	ps3_flip_completed = 0;
	gcmSetFlipHandler( PS3_RSX_FlipHandler );

	ps3_current_fb = 0;
	ps3_rsx_ready = true;
	Con_Printf( "PS3_RSX_Init: ready, %ux%u\n", ps3_display_width, ps3_display_height );

	return true;
}

void Platform_Minimize_f( void )
{
	// Stub: no windowing system to minimize into.
}

platform_orientation_t Platform_GetDisplayOrientation( void )
{
	// Fixed-orientation TV signal; in_gyro.c calls this unconditionally, not just under SDL.
	return ORIENTATION_LANDSCAPE;
}

void VID_Info_f( void )
{
	// Every non-SDL platform needs its own "vid_info" implementation.
	Con_Printf( "Video: " S_GREEN "PS3 (RSX)" S_DEFAULT "\n" );
	Con_Printf( "Window size: " S_GREEN "%ux%u" S_DEFAULT "\n", ps3_display_width, ps3_display_height );
}

// REF_GL only. Opens the next frame: waits for the buffer, re-points the RSX,
// resets ps3gl's per-frame state. Called from GL_SwapBuffers (and once from
// R_Init_Video for the first frame) since ref_gl gives only one per-frame hook.
static void PS3_GL_BeginFrame( void )
{
	PS3_RSX_WaitForFreeBuffer();
	PS3_RSX_SetRenderTarget( ps3_current_fb );
	ps3gl_begin_frame();
}

void GL_SwapBuffers( void )
{
	if( !ps3_rsx_ready )
		return;

	// Writes the vertex-ring fence label so the next BeginFrame knows it's safe to reuse.
	if( ps3_gl_mode )
		ps3gl_end_frame();

	// WaitFlip before SetFlip, opposite of rsx.h's own doc comment -- the doc order
	// produced a black screen on real hardware.
	gcmSetWaitFlip( ps3_gcm_context );
	gcmSetFlip( ps3_gcm_context, ps3_current_fb );
	rsxFlushBuffer( ps3_gcm_context );

	ps3_flip_queued++;
	ps3_current_fb = ( ps3_current_fb + 1 ) % PS3_FB_COUNT;
	PS3_DIAG_FRAME( "vid: GL_SwapBuffers exit, queued=%u", (unsigned int)ps3_flip_queued );

	if( ps3_gl_mode )
		PS3_GL_BeginFrame();
}

qboolean R_Init_Video( ref_graphic_apis_t type )
{
	if( type != REF_SOFTWARE && type != REF_GL )
		return false;

	ps3_gl_mode = ( type == REF_GL );

	// Let the renderer request context attributes before the context exists.
	// Nothing is negotiable on RSX (GL_SetAttribute is a no-op), but this is
	// also where ref_gl decides glw_state.extended, so it must still run.
	if( ps3_gl_mode )
		ref.dllFuncs.GL_SetupAttributes( glw_state.safe );

	if( !VID_SetMode( )) // runs PS3_RSX_Init via R_ChangeDisplaySettings
		return false;

	if( ps3_gl_mode )
	{
		// Everything below is what ref_soft's presentation-only path skips.
		if( !PS3_RSX_AllocDepthBuffer( ))
			return false;

		if( !ps3gl_init( ps3_gcm_context, ps3_display_width, ps3_display_height ))
		{
			Con_Printf( "R_Init_Video: ps3gl_init FAILED\n" );
			return false;
		}

		// Open the first frame here; every later one is opened by GL_SwapBuffers.
		PS3_GL_BeginFrame();

		// MANDATORY: sets glw_state.initialized, which GL_UploadTexture checks first.
		// Missing it silently skips every texture upload (HW-confirmed: untextured render).
		ref.dllFuncs.GL_InitExtensions();

		Con_Printf( "R_Init_Video: ref_gl ready on RSX, %ux%u\n",
			ps3_display_width, ps3_display_height );
	}

	// Nothing drains the sysutil event queue before the first Host_Frame, and this
	// function is the other real chunk of time in that window besides FS checks.
	PS3_CheckExitRequested();

	host.renderinfo_changed = false;
	return true;
}

void R_Free_Video( void )
{
	int i;

	PS3_DIAG( "R_Free_Video: enter" );

	if( ps3_gl_mode )
	{
		ps3gl_shutdown();
		ps3_gl_mode = false;
	}

	PS3_DIAG( "R_Free_Video: after ps3gl_shutdown" );

	if( ps3_rsx_ready )
	{
		// Drain the FIFO and block until the RSX is idle before freeing memory it may
		// still reference -- else rsxFree() below races the GPU and hard-freezes.
		rsxFinish( ps3_gcm_context, 1 );

		PS3_DIAG( "R_Free_Video: after rsxFinish" );

		// Unregister the flip handler before tearing down GCM host memory below --
		// a flip interrupt after ps3_rsx_host_addr is freed would run against torn-down state.
		gcmSetFlipHandler( NULL );

		PS3_DIAG( "R_Free_Video: after gcmSetFlipHandler(NULL)" );

		for( i = 0; i < PS3_FB_COUNT; i++ )
		{
			if( ps3_color_buffer[i] )
				rsxFree( ps3_color_buffer[i] );
		}

		PS3_DIAG( "R_Free_Video: after color buffer rsxFree loop" );

		if( ps3_depth_buffer )
		{
			rsxFree( ps3_depth_buffer );
			ps3_depth_buffer = NULL;
		}
		ps3_current_rt = -1;

		ps3_rsx_ready = false;

		PS3_DIAG( "R_Free_Video: after depth buffer rsxFree" );
	}

	if( ps3_rsx_host_addr )
	{
		free( ps3_rsx_host_addr );
		ps3_rsx_host_addr = NULL;
	}

	PS3_DIAG( "R_Free_Video: after free(ps3_rsx_host_addr)" );

	if( ps3_swbuffer )
	{
		free( ps3_swbuffer );
		ps3_swbuffer = NULL;
	}

	ref.dllFuncs.GL_ClearExtensions();

	PS3_DIAG( "R_Free_Video: exit" );
}

qboolean VID_SetMode( void )
{
	R_ChangeDisplaySettings( PS3_SCREEN_WIDTH, PS3_SCREEN_HEIGHT, WINDOW_MODE_FULLSCREEN );
	return true;
}

rserr_t R_ChangeDisplaySettings( int width, int height, window_mode_t window_mode )
{
	// Resolution is fixed by the TV -- PS3_RSX_Init queries the real mode and
	// overwrites ps3_display_width/height with it, never a guessed one.
	if( !PS3_RSX_Init( ))
		return rserr_unknown;

	R_SaveVideoMode( ps3_display_width, ps3_display_height, ps3_display_width, ps3_display_height, false );
	return rserr_ok;
}

int GL_SetAttribute( int attr, int val )
{
	// Nothing negotiable: the RSX surface format is fixed by PS3_RSX_SetRenderTarget.
	return 0;
}

int GL_GetAttribute( int attr, int *val )
{
	if( !val )
		return -1;

	// Reports what PS3_RSX_SetRenderTarget actually configures, not zeros.
	switch( attr )
	{
	case REF_GL_RED_SIZE:
	case REF_GL_GREEN_SIZE:
	case REF_GL_BLUE_SIZE:
	case REF_GL_ALPHA_SIZE:
		*val = 8;
		return 0;
	case REF_GL_DEPTH_SIZE:
		*val = 24;
		return 0;
	case REF_GL_STENCIL_SIZE:
		*val = 8;
		return 0;
	case REF_GL_DOUBLEBUFFER:
		*val = 1;
		return 0;
	case REF_GL_MULTISAMPLESAMPLES:
		*val = 0; // GCM_SURFACE_CENTER_1
		return 0;
	default:
		*val = 0;
		return -1;
	}
}

int R_MaxVideoModes( void )
{
	return 0;
}

vidmode_t *R_GetVideoMode( int num )
{
	return NULL;
}

void *GL_GetProcAddress( const char *name )
{
	return NULL;
}

static qboolean vsync;

void GL_UpdateSwapInterval( void )
{
	if( FBitSet( gl_vsync.flags, FCVAR_CHANGED ))
	{
		ClearBits( gl_vsync.flags, FCVAR_CHANGED );
		vsync = gl_vsync.value;
	}
}

void *SW_LockBuffer( void )
{
	return ps3_swbuffer;
}

void SW_UnlockBuffer( void )
{
	byte *dst;
	uint  row;
	uint  src_pitch;
	static qboolean logged_skip;
	static uint      call_count;

	if( !ps3_rsx_ready || !ps3_swbuffer )
	{
		// Proves whether frames reach this function with RSX unready, vs. never being called.
		if( !logged_skip )
		{
			logged_skip = true;
			Con_Printf( "SW_UnlockBuffer: skipped (ps3_rsx_ready=%d ps3_swbuffer=%p)\n",
				(int)ps3_rsx_ready, (void *)ps3_swbuffer );
		}
		return;
	}

	if( call_count == 0 )
		Con_Printf( "SW_UnlockBuffer: first call, fb=%d %ux%u\n", ps3_current_fb, ps3_display_width, ps3_display_height );
	call_count++;

	PS3_DIAG_FRAME( "vid: SW_UnlockBuffer before WaitForFreeBuffer, fb=%d", ps3_current_fb );
	PS3_RSX_WaitForFreeBuffer();
	PS3_DIAG_FRAME( "vid: SW_UnlockBuffer after WaitForFreeBuffer" );

	// ps3_swbuffer's ARGB8888 layout already matches GCM_SURFACE_A8R8G8B8's byte
	// order, so this is a straight per-row copy -- rows differ since XDR is tightly
	// packed but the RSX buffer's pitch is 64-byte aligned.
	dst = (byte *)ps3_color_buffer[ps3_current_fb];
	src_pitch = ps3_display_width * 4;

	for( row = 0; row < ps3_display_height; row++ )
	{
		memcpy( dst + row * ps3_color_pitch, ps3_swbuffer + row * src_pitch, src_pitch );
	}

	// Order PPE stores before the RSX doorbell write GL_SwapBuffers issues.
	__asm__ __volatile__( "sync" ::: "memory" );

	PS3_DIAG_FRAME( "vid: SW_UnlockBuffer copy done, before GL_SwapBuffers" );
	GL_SwapBuffers();
	PS3_DIAG_FRAME( "vid: SW_UnlockBuffer exit" );
}

qboolean SW_CreateBuffer( int width, int height, uint *stride, uint *bpp, uint *r, uint *g, uint *b )
{
	*stride = width;
	*bpp = 4;
	*r = 0x00ff0000;
	*g = 0x0000ff00;
	*b = 0x000000ff;

	// 128-byte alignment: PPE cache line size and SPE/RSX DMA-friendly
	ps3_swbuffer = memalign( 128, width * height * 4 );
	if( !ps3_swbuffer )
		return false;

	// memalign doesn't zero -- untouched regions would show garbage from the fresh
	// XDR allocation (confirmed on HW: visible noise on the first rendered frame).
	memset( ps3_swbuffer, 0, width * height * 4 );

	return true;
}

ref_window_type_t R_GetWindowHandle( void **handle, ref_window_type_t type )
{
	return REF_WINDOW_TYPE_NULL;
}

#endif // XASH_VIDEO == VIDEO_PS3
