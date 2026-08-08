/*
sys_ps3.c - PS3 (PSL1GHT) system glue
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
#include "ps3_diag.h"
#include "ps3_osk.h"
#include <sys/process.h>
// net/net.h declares foo() instead of foo(void); silence -Werror=strict-prototypes for it only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <net/net.h>
#pragma GCC diagnostic pop
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ppu-types.h>
// Same -Werror=strict-prototypes issue as net/net.h above.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <sysutil/sysutil.h>
#pragma GCC diagnostic pop
#include <sysmodule/sysmodule.h>

// BSP/zone recursion needs more than the default stack; 1MB main thread stack
SYS_PROCESS_PARAM( 1001, 0x100000 )

// Platform_DoubleTime/Platform_Sleep come from platform/posix/sys_posix.c (also
// compiled for PS3); don't redefine them here. Platform_ShellExecute needs
// fork()/execvp(), unavailable on this libc, so PS3 gets its own stub instead.
void Platform_ShellExecute( const char *path, const char *parms )
{
	Con_Printf( "Platform_ShellExecute: not supported on PS3 (%s %s)\n", path, parms );
}

// UDP debug log sink -- only observability channel before video bring-up.
// EDIT the IP below per dev machine before flashing a build.
#define PS3_DEBUG_LOG_IP    "192.168.178.99"
#define PS3_DEBUG_LOG_PORT  18194

static int ps3_log_socket = -1;
static struct sockaddr_in ps3_log_addr;

void PS3_Printf( const char *fmt, ... ) FORMAT_CHECK( 1 );

void PS3_Printf( const char *fmt, ... )
{
	// static, not a stack local: an 8KB frame this deep in the Con_Printf call
	// chain overran the real thread stack and clobbered .bss (confirmed on HW).
	static char buffer[MAX_PRINT_MSG];
	va_list ap;

	if( ps3_log_socket < 0 )
		return;

	va_start( ap, fmt );
	Q_vsnprintf( buffer, sizeof( buffer ), fmt, ap );
	va_end( ap );

	sendto( ps3_log_socket, buffer, Q_strlen( buffer ), 0,
		(struct sockaddr *)&ps3_log_addr, sizeof( ps3_log_addr ));
}

// Diagnostic channel -- see public/ps3_diag.h for why this exists.
unsigned int ps3_diag_seq = 0;
int          ps3_diag_enabled = 0;
int          ps3_diag_budget = 8000;
int          ps3_diag_frames = 0;

// Per-frame CPU phase accumulators, reset by Host_Frame after reporting (see ps3_diag.h).
double ps3_phase_input  = 0.0;
double ps3_phase_server = 0.0;
double ps3_phase_client = 0.0;
double ps3_phase_render = 0.0;
double ps3_phase_sound  = 0.0;

// Sub-phases of ps3_phase_server: `think` is game-DLL entity thinking, `link` is
// SV_LinkEdict's areanode relink, the remainder of `server` is networking/rest.
double ps3_phase_sv_think = 0.0;
double ps3_phase_sv_link  = 0.0;
unsigned int ps3_phase_sv_think_calls = 0;
unsigned int ps3_phase_sv_link_calls  = 0;

// Round 2: think/link are flat and tiny, `rest` carries the whole framerate swing.
// These name the three top-level Host_ServerFrame calls `rest` is made of.
double ps3_phase_sv_readpackets = 0.0;
double ps3_phase_sv_gameframe   = 0.0;
double ps3_phase_sv_sendmsgs    = 0.0;

// Round 3: sendmsgs was 79% of the server frame, view-independent snapshot cost.
// `addents` is SV_AddEntitiesToPacket, `emit` is SV_EmitPacketEntities's delta encode.
double ps3_phase_sv_addents = 0.0;
double ps3_phase_sv_emit    = 0.0;
unsigned int ps3_phase_sv_addents_calls = 0;
unsigned int ps3_phase_sv_fullpack_calls = 0;

// Round 4: emit's ~31us/entity is too slow for mostly-unchanged fields. These
// distinguish wiped delta encoders (written/visited ~1.0, with_encoder 0) from
// a costly-but-working suppression scan (written/visited small).
unsigned int ps3_delta_fields_visited = 0;
unsigned int ps3_delta_fields_written = 0;
unsigned int ps3_delta_entities       = 0;
unsigned int ps3_delta_with_encoder   = 0;

// Round 5: suppression was fine, so the unmeasured cost is SV_FindBestBaseline's
// up to 63-state backward scan; `tests` x 52 is the true field-compare count.
unsigned int ps3_baseline_calls    = 0;
unsigned int ps3_baseline_tests    = 0;
unsigned int ps3_baseline_iters    = 0;
unsigned int ps3_baseline_newents  = 0;
unsigned int ps3_baseline_num_instanced = 0;

static uintptr_t ps3_diag_stack_top = 0;

void PS3_Diag( const char *fmt, ... )
{
	// static for the same reason PS3_Printf's buffer is (see above).
	static char text[MAX_PRINT_MSG];
	static char line[MAX_PRINT_MSG];
	va_list ap;
	int len;

	if( !ps3_diag_enabled || ps3_diag_budget <= 0 || ps3_log_socket < 0 )
		return;

	ps3_diag_budget--;

	va_start( ap, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, ap );
	va_end( ap );

	// The sequence number is the point: a gap means a dropped datagram, a clean
	// stop means the code after the last line never executed.
	len = Q_snprintf( line, sizeof( line ), "DIAG %05u f=%d %s\n",
		++ps3_diag_seq, (int)host.framecount, text );

	if( len < 0 )
		len = Q_strlen( line );

	sendto( ps3_log_socket, line, len, 0,
		(struct sockaddr *)&ps3_log_addr, sizeof( ps3_log_addr ));
}

// Deliberately ungated -- the reference point must be recorded every frame.
void PS3_DiagStackTop( const void *probe )
{
	ps3_diag_stack_top = (uintptr_t)probe;
}

// A *watermark*, not an execution marker -- only emits on a new max, so a missing
// STACK line does not mean the call site didn't run (PS3_DIAG markers track that).
void PS3_DiagStack( const char *label, const void *probe )
{
	static long deepest = 0;
	uintptr_t   here = (uintptr_t)probe;
	long        used;

	if( !ps3_diag_enabled || ps3_diag_budget <= 0 )
		return;

	// PPC stacks grow down, so the frame loop's probe sits above this one.
	if( ps3_diag_stack_top >= here )
		used = (long)( ps3_diag_stack_top - here );
	else used = -(long)( here - ps3_diag_stack_top );

	if( used <= deepest )
		return;
	deepest = used;

	// Limit is SYS_PROCESS_PARAM's stacksize argument at the top of this file.
	PS3_Diag( "STACK %s used=%ld of 1048576 (new max)", label, used );
}

// Memory probe: measures the LARGEST CONTIGUOUS free block (not total free bytes),
// since realloc() needs one run of address space. Plain malloc/free, not
// Mem_Alloc/zone.c, so the probe itself can't trip Sys_Error or perturb the heap.
static size_t PS3_LargestContiguousBlock( void )
{
	size_t lo = 0;
	size_t hi = 256u * 1024u * 1024u; // full XDR size, known-unreachable upper bound
	size_t best = 0;

	// Binary search: 28 iterations narrows 256MB to within ~1KB, plenty for a diagnostic.
	for( int i = 0; i < 28 && lo < hi; i++ )
	{
		size_t mid = lo + ( hi - lo ) / 2;
		void *p = malloc( mid );

		if( p )
		{
			free( p );
			best = mid;
			lo = mid + 1;
		}
		else hi = mid;
	}

	return best;
}

// Called once at boot and once per map spawn, so a post-SP-session number can
// be compared against the boot baseline instead of guessing at fragmentation.
void PS3_ProbeMemory( const char *label )
{
	size_t largest = PS3_LargestContiguousBlock();

	PS3_Printf( "PS3_ProbeMemory[%s]: largest contiguous block ~%s\n",
		label, Q_memprint( (float)largest ));
}

// Exit-chain probe (constructor(101) + atexit()) REMOVED 2026-07-28 -- it hung
// a fresh power-cycle boot before PS3_Init ran. Do NOT re-add this constructor-
// priority trick; a plain atexit() from inside main() is the safe fallback.

// XMB exit delivers SYSUTIL_EXIT_GAME here; this only latches a flag -- the
// real Sys_Quit() runs from PS3_CheckExitRequested (in_ps3.c) at normal frame depth.
// Without this, LV2/XMB hard-freezes waiting for the exit event to drain.
static volatile int ps3_exit_requested = 0;

static void PS3_SysutilCallback( u64 status, u64 param, void *userdata )
{
	(void)userdata;

	if( status == SYSUTIL_EXIT_GAME )
	{
		ps3_exit_requested = 1;
		return;
	}

	// Native OSK shares SYSUTIL_EVENT_SLOT0 rather than taking a second slot.
	PS3_OSK_SysutilCallback( status, param );
}

void PS3_CheckExitRequested( void )
{
	sysUtilCheckCallback();

	if( ps3_exit_requested )
	{
		ps3_exit_requested = 0;
		Sys_Quit( "XMB exit" );
	}
}

void PS3_Init( void )
{
	int net_ret;

	sysUtilRegisterCallback( SYSUTIL_EVENT_SLOT0, PS3_SysutilCallback, NULL );

	// The PSL1GHT net module must be loaded before netInitialize() -- confirmed
	// against the hardware-validated sibling ioQuake3-PS3 port.
	sysModuleLoad( SYSMODULE_NET );

	net_ret = netInitialize();
	(void)net_ret; // only read below when the UDP log sink is compiled in

#if XASH_PS3_UDP_LOG
	// Gated behind --enable-ps3-udp-log: opens an unauthenticated UDP sender to a
	// hardcoded dev IP, no business in a release build.
	ps3_log_socket = socket( AF_INET, SOCK_DGRAM, 0 );
	if( ps3_log_socket >= 0 )
	{
		memset( &ps3_log_addr, 0, sizeof( ps3_log_addr ));
		ps3_log_addr.sin_family = AF_INET;
		ps3_log_addr.sin_port = htons( PS3_DEBUG_LOG_PORT );
		ps3_log_addr.sin_addr.s_addr = inet_addr( PS3_DEBUG_LOG_IP );

		PS3_Printf( "PS3_Init: UDP debug log online (sizeof(void*)=%u, sizeof(long)=%u)\n",
			(unsigned int)sizeof( void * ), (unsigned int)sizeof( long ));

		// If boot hangs before this line prints, the hang is inside sysModuleLoad/
		// netInitialize itself, not anything after it.
		PS3_Printf( "PS3_Init: netInitialize ret=%d\n", net_ret );
	}
#endif

	PS3_ProbeMemory( "boot" );

	PS3_OSK_Init();
}

void PS3_Shutdown( void )
{
	PS3_OSK_Shutdown();

#if XASH_PS3_UDP_LOG
	if( ps3_log_socket >= 0 )
	{
		PS3_Printf( "PS3_Shutdown: closing UDP debug log\n" );

		close( ps3_log_socket );
		ps3_log_socket = -1;
	}
#endif

	// netDeinitialize() REMOVED 2026-07-27 -- caused a hardware-confirmed shutdown
	// freeze. If the symptom it "fixed" recurs, use sysModuleUnload(SYSMODULE_NET)
	// paired with PS3_Init's sysModuleLoad, not netDeinitialize() alone.
}

// Headless FS validation, called once after the gamedir is mounted. Diagnostic
// only (never Sys_Error) -- a missing file just shows as MISSING in the UDP log.
void PS3_VerifyGameAssets( void )
{
	static const char *const core_assets[] =
	{
		"liblist.gam",
		"gfx/conchars",
		"gfx.wad",
	};
	int i;

	for( i = 0; i < (int)( sizeof( core_assets ) / sizeof( core_assets[0] )); i++ )
	{
		dword crc;

		if( !FS_FileExists( core_assets[i], false ))
		{
			Con_Printf( "PS3_VerifyGameAssets: %s MISSING\n", core_assets[i] );
			continue;
		}

		if( CRC32_File( &crc, core_assets[i] ))
			Con_Printf( "PS3_VerifyGameAssets: %s CRC32=0x%08X\n", core_assets[i], crc );
		else
			Con_Printf( "PS3_VerifyGameAssets: %s CRC32 FAILED\n", core_assets[i] );
	}
}
