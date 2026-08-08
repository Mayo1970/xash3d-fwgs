/*
ps3_diag.h - PS3 hardware-freeze diagnostic channel
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
#ifndef PS3_DIAG_H
#define PS3_DIAG_H

#include "xash3d_types.h"

// No debugger for retail PS3 homebrew; an invalid access silently hard-locks the
// console, so UDP marker logging is the only way to locate a freeze. One global
// sequence number + one global budget + an explicit enable flag (vs. earlier
// per-site counters, which made "went silent" ambiguous between dead code and
// an exhausted budget). Also provides a stack watermark against the 1MB main stack.

#if XASH_PS3

extern unsigned int ps3_diag_seq;      // monotonic, every emitted line
extern int          ps3_diag_enabled;  // 0 at boot, raised at map load
extern int          ps3_diag_budget;   // global line budget, counts down
extern int          ps3_diag_frames;   // 0 by default: gates the per-frame sites separately

void PS3_Diag( const char *fmt, ... ) FORMAT_CHECK( 1 );
void PS3_DiagStackTop( const void *probe );
void PS3_DiagStack( const char *label, const void *probe );

#define PS3_DIAG( ... ) PS3_Diag( __VA_ARGS__ )
#define PS3_DIAG_ENABLE() ( ps3_diag_enabled = 1 )

// Arm/re-arm with a fresh budget and sequence number per investigated map load.
#define PS3_DIAG_REARM() ( ps3_diag_enabled = 1, ps3_diag_budget = 8000, ps3_diag_seq = 0 )

// Same channel, split out for per-frame sites -- else they'd starve the
// one-shot map-spawn markers' shared budget within seconds.
#define PS3_DIAG_FRAME( ... ) do { if( ps3_diag_frames ) PS3_Diag( __VA_ARGS__ ); } while( 0 )

// Records the frame loop's stack address; everything below is measured as a delta.
#define PS3_DIAG_STACK_TOP() \
	do { char ps3_diag_probe_; PS3_DiagStackTop( &ps3_diag_probe_ ); } while( 0 )

// Must be a macro -- a plain function would only ever measure its own depth.
#define PS3_DIAG_STACK( label ) \
	do { char ps3_diag_probe_; PS3_DiagStack(( label ), &ps3_diag_probe_ ); } while( 0 )

// Coarse per-frame CPU phase accumulators (sys_ps3.c owns the definitions).
// HW measurement established the ~50fps floor is CPU-bound (flip wait reads
// 0.00ms there) and draw-call count is not the cause. Always-on continuous
// telemetry, unlike PS3_DIAG_FRAME's budgeted one-shot channel.
extern double ps3_phase_input;
extern double ps3_phase_server;
extern double ps3_phase_client;
extern double ps3_phase_render;
extern double ps3_phase_sound;

// Sub-phases of ps3_phase_server: `think` is SV_RunThink, `link` is SV_LinkEdict's
// areanode relink; server - think - link is packet read/send and the rest.
extern double ps3_phase_sv_think;
extern double ps3_phase_sv_link;
extern unsigned int ps3_phase_sv_think_calls;
extern unsigned int ps3_phase_sv_link_calls;

// The three top-level Host_ServerFrame calls splitting `rest`: gameframe is
// SV_RunGameFrame (includes think/link), sendmsgs is SV_SendClientMessages,
// readpackets is SV_ReadPackets.
extern double ps3_phase_sv_readpackets;
extern double ps3_phase_sv_gameframe;
extern double ps3_phase_sv_sendmsgs;

// Splits sendmsgs (measured 79% of the server frame): addents is
// SV_AddEntitiesToPacket, emit is SV_EmitPacketEntities's delta encode.
extern double ps3_phase_sv_addents;
extern double ps3_phase_sv_emit;
extern unsigned int ps3_phase_sv_addents_calls;
extern unsigned int ps3_phase_sv_fullpack_calls;

// Delta-encode field accounting: written/visited shows if suppression is working;
// with_encoder shows if the game DLL's encoder callbacks survived (0 = wiped).
extern unsigned int ps3_delta_fields_visited;
extern unsigned int ps3_delta_fields_written;
extern unsigned int ps3_delta_entities;
extern unsigned int ps3_delta_with_encoder;

// SV_FindBestBaseline accounting: tests x ~52 fields is the real compare count
// the write-pass counters above miss. num_instanced == 0 means every new
// entity paid the full backward search.
extern unsigned int ps3_baseline_calls;
extern unsigned int ps3_baseline_tests;
extern unsigned int ps3_baseline_iters;
extern unsigned int ps3_baseline_newents;
extern unsigned int ps3_baseline_num_instanced;

// Usage: double t0 = PS3_PHASE_NOW(); ...work...; PS3_PHASE_ADD( acc, t0 );
// Every call site already includes platform/platform.h for Platform_DoubleTime.
#define PS3_PHASE_NOW()          Platform_DoubleTime()
#define PS3_PHASE_ADD( acc, t0 ) ( (acc) += Platform_DoubleTime() - (t0) )

// Time a single call and count it, without needing an #if at the call site.
#define PS3_PHASE_SCOPE( acc, calls, call ) \
	do { \
		double ps3_scope_t0_ = PS3_PHASE_NOW(); \
		call; \
		PS3_PHASE_ADD( acc, ps3_scope_t0_ ); \
		(calls)++; \
	} while( 0 )

#else // !XASH_PS3

#define PS3_DIAG( ... )        ((void)0)
#define PS3_DIAG_FRAME( ... )  ((void)0)
#define PS3_DIAG_ENABLE()      ((void)0)
#define PS3_DIAG_REARM()       ((void)0)
#define PS3_DIAG_STACK_TOP()   ((void)0)
#define PS3_DIAG_STACK( label ) ((void)0)
#define PS3_PHASE_NOW()          0.0
#define PS3_PHASE_ADD( acc, t0 ) ((void)0)
#define PS3_PHASE_SCOPE( acc, calls, call ) do { call; } while( 0 )

// Shared engine code references these unconditionally; map to a discarded dummy off-PS3.
extern unsigned int ps3_diag_dummy_counter;
#define ps3_delta_fields_visited ps3_diag_dummy_counter
#define ps3_delta_fields_written ps3_diag_dummy_counter
#define ps3_delta_entities       ps3_diag_dummy_counter
#define ps3_delta_with_encoder   ps3_diag_dummy_counter
#define ps3_baseline_calls       ps3_diag_dummy_counter
#define ps3_baseline_tests       ps3_diag_dummy_counter
#define ps3_baseline_iters       ps3_diag_dummy_counter
#define ps3_baseline_newents     ps3_diag_dummy_counter
#define ps3_baseline_num_instanced ps3_diag_dummy_counter

#endif // XASH_PS3

#endif // PS3_DIAG_H
