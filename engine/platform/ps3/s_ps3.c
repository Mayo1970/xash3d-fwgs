/*
s_ps3.c - PS3 (PSL1GHT) audio backend
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

#include "common.h"
#include "platform.h"
#include "sound.h"
#include "voice.h"
#include "ps3_diag.h"

// Same -Werror=strict-prototypes fixup as net/net.h in sys_ps3.c, applied defensively here too.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sysmodule/sysmodule.h>
#pragma GCC diagnostic pop

// PSL1GHT's audio port is a hard 48kHz float32/256-frame-block contract, so int16
// snd.buffer is converted in a dedicated feeder thread that ONLY copies a pre-mixed
// block -- never mix/decode there, or a missed ~5.3ms deadline clicks.
// Unlike ioQuake3-PS3, Xash's backend is pull/poll: the engine owns snd.buffer and
// the backend advances snd.samplepos itself; snd.format.speed can honestly be 48000.

#define PS3_AUDIO_RATE       48000
#define PS3_AUDIO_CHANNELS   2
#define PS3_AUDIO_BLOCK_SIZE 256 // PS3 hardware block size, in frames

static audioPortConfig  ps3_audio_config;
static u32               ps3_audio_port;
static qboolean           ps3_audio_running;

static sys_event_queue_t ps3_audio_eventQ;
static sys_ipc_key_t     ps3_audio_queueKey;
static sys_ppu_thread_t  ps3_audio_thread;
static volatile int      ps3_audio_quit;

// Mixer starvation: emit silence, never stale ring content. The real mixahead
// cushion is ~110ms (44100-based cvar name, but this port outputs 48000). A stalled
// main thread longer than that used to walk the feeder's read pointer past the mix
// frontier into a 682ms-old ring lap, looping stale audio forever. Now it consults
// snd.paintedtime and only consumes what was really mixed, zero-filling the rest --
// samplepos stays parked at the mix frontier so S_GetSoundtime resumes cleanly.
// PS3-only: desktop backends don't see multi-second synchronous filesystem stalls.
static volatile u32 ps3_audio_silentBlocks; // blocks padded with silence
static volatile u32 ps3_audio_silentFrames; // total frames of silence emitted
static volatile u32 ps3_audio_longestGap;   // longest unbroken run of silent frames
static volatile u32 ps3_audio_dropouts;     // discontinuities >1s (map loads etc)
static u32          ps3_audio_curGap;       // run in progress, feeder thread only
static u32          ps3_audio_playedFrames;
static qboolean     ps3_audio_primed;

static void PS3_Audio_ThreadFunc( void *arg )
{
	sys_event_t event;
	volatile u64 *readIndexPtr = (volatile u64 *)(uintptr_t)ps3_audio_config.readIndex;
	f32 *dataStart = (f32 *)(uintptr_t)ps3_audio_config.audioDataStart;
	u64 numBlocks = ps3_audio_config.numBlocks;
	// audioDataStart's alignment isn't guaranteed; float stores to a misaligned
	// address fault/corrupt on this ABI, so stage into an aligned buffer first.
	static f32 staging[PS3_AUDIO_BLOCK_SIZE * PS3_AUDIO_CHANNELS] __attribute__(( aligned( 16 )));

	(void)arg;

	if( !readIndexPtr || !dataStart || !numBlocks )
	{
		Con_Printf( "PS3_Audio: bad port config pointers, feeder thread exiting\n" );
		sysThreadExit( 1 );
		return;
	}

	while( !ps3_audio_quit )
	{
		s32 ret = sysEventQueueReceive( ps3_audio_eventQ, &event, 20 * 1000 );

		if( ret != 0 )
			continue; // timeout, just recheck the quit flag

		if( ps3_audio_quit )
			break;

		u64 currentBlock = *readIndexPtr;
		u32 writeBlock = (u32)(( currentBlock + 1 ) % numBlocks );
		f32 *dst = dataStart + writeBlock * PS3_AUDIO_CHANNELS * PS3_AUDIO_BLOCK_SIZE;

		int frame, ch;
		int monoSamples = snd.samples; // total mono (interleaved) samples in snd.buffer
		int pos = snd.samplepos;
		int real;                      // frames of genuinely-mixed audio this block may consume

		// Frames mixed minus frames already handed to hardware; unsigned subtraction
		// stays correct across snd.paintedtime's 32-bit wrap.
		{
			s32 margin = (s32)((u32)snd.paintedtime - ps3_audio_playedFrames );

			if( margin < -PS3_AUDIO_RATE )
			{
				// >1s behind, or paintedtime was reset (map load/engine restart) --
				// resync instead of counting thousands of blocks, but record it.
				ps3_audio_dropouts++;
				ps3_audio_playedFrames = (u32)snd.paintedtime;
				ps3_audio_primed = false;
				real = 0;
			}
			else if( !ps3_audio_primed )
			{
				// Wait for the mixer to genuinely get ahead once before trusting margin,
				// emitting silence rather than handing out an unwritten ring.
				if( margin >= PS3_AUDIO_BLOCK_SIZE )
				{
					ps3_audio_primed = true;
					real = PS3_AUDIO_BLOCK_SIZE;
				}
				else real = 0;
			}
			else
			{
				real = bound( 0, (int)margin, PS3_AUDIO_BLOCK_SIZE );

				if( real < PS3_AUDIO_BLOCK_SIZE )
				{
					ps3_audio_silentBlocks++;
					ps3_audio_silentFrames += PS3_AUDIO_BLOCK_SIZE - real;
				}
			}
		}

		// Longest unbroken silence run, not worst-per-block deficit: a per-block
		// figure pins at PS3_AUDIO_BLOCK_SIZE the moment the mixer stops outright.
		if( real < PS3_AUDIO_BLOCK_SIZE )
		{
			ps3_audio_curGap += PS3_AUDIO_BLOCK_SIZE - real;

			if( ps3_audio_curGap > ps3_audio_longestGap )
				ps3_audio_longestGap = ps3_audio_curGap;
		}
		else ps3_audio_curGap = 0;

		for( frame = 0; frame < real; frame++ )
		{
			for( ch = 0; ch < PS3_AUDIO_CHANNELS; ch++ )
			{
				short *src = (short *)snd.buffer;
				int idx = pos;

				if( idx >= monoSamples )
					idx -= monoSamples;

				staging[frame * PS3_AUDIO_CHANNELS + ch] = src[idx] * ( 1.0f / 32768.0f );
				pos++;

				if( pos >= monoSamples )
					pos = 0;
			}
		}

		// Silence for whatever the mixer did not produce (see block comment above).
		if( real < PS3_AUDIO_BLOCK_SIZE )
		{
			memset( &staging[real * PS3_AUDIO_CHANNELS], 0,
				( PS3_AUDIO_BLOCK_SIZE - real ) * PS3_AUDIO_CHANNELS * sizeof( staging[0] ));
		}

		memcpy( dst, staging, sizeof( staging ));

		// Advance by what was really consumed, parking at the mix frontier so
		// S_GetSoundtime reports a soundtime that also stopped.
		snd.samplepos = pos;
		ps3_audio_playedFrames += real;
	}

	sysThreadExit( 0 );
}

qboolean SNDDMA_Init( void )
{
	audioPortParam params;
	s32 ret;
	int samplecount;

	ret = sysModuleLoad( SYSMODULE_AUDIO );
	if( ret != 0 && ret != SYSMODULE_ERR_DUPLICATE )
	{
		Con_Printf( "SNDDMA_Init: sysModuleLoad(AUDIO) failed: 0x%08x\n", (unsigned int)ret );
		return false;
	}

	ret = audioInit();
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: audioInit failed: 0x%08x\n", (unsigned int)ret );
		return false;
	}

	memset( &params, 0, sizeof( params ));
	params.numChannels = AUDIO_PORT_2CH;
	params.numBlocks = AUDIO_BLOCK_8;
	params.attrib = AUDIO_PORT_INITLEVEL;
	params.level = 1.0f;

	ret = audioPortOpen( &params, &ps3_audio_port );
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: audioPortOpen failed: 0x%08x\n", (unsigned int)ret );
		audioQuit();
		return false;
	}

	ret = audioGetPortConfig( ps3_audio_port, &ps3_audio_config );
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: audioGetPortConfig failed: 0x%08x\n", (unsigned int)ret );
		audioPortClose( ps3_audio_port );
		audioQuit();
		return false;
	}

	ret = audioCreateNotifyEventQueue( &ps3_audio_eventQ, &ps3_audio_queueKey );
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: audioCreateNotifyEventQueue failed: 0x%08x\n", (unsigned int)ret );
		audioPortClose( ps3_audio_port );
		audioQuit();
		return false;
	}

	ret = audioSetNotifyEventQueue( ps3_audio_queueKey );
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: audioSetNotifyEventQueue failed: 0x%08x\n", (unsigned int)ret );
		sysEventQueueDestroy( ps3_audio_eventQ, 0 );
		audioPortClose( ps3_audio_port );
		audioQuit();
		return false;
	}

	sysEventQueueDrain( ps3_audio_eventQ );

	ret = audioPortStart( ps3_audio_port );
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: audioPortStart failed: 0x%08x\n", (unsigned int)ret );
		audioRemoveNotifyEventQueue( ps3_audio_queueKey );
		sysEventQueueDestroy( ps3_audio_eventQ, 0 );
		audioPortClose( ps3_audio_port );
		audioQuit();
		return false;
	}

	snd.format.speed = PS3_AUDIO_RATE;
	snd.format.channels = PS3_AUDIO_CHANNELS;
	snd.format.width = 2;

	samplecount = s_samplecount.value;
	if( !samplecount )
		samplecount = 0x8000;

	snd.samples = samplecount * PS3_AUDIO_CHANNELS;
	snd.buffer = Mem_Calloc( sndpool, snd.samples * 2 );
	snd.samplepos = 0;
	snd.backend_name = "PS3 (PSL1GHT audio port)";
	snd.initialized = true;

	ps3_audio_quit = 0;
	ps3_audio_running = true;

	ps3_audio_silentBlocks = 0;
	ps3_audio_silentFrames = 0;
	ps3_audio_longestGap = 0;
	ps3_audio_curGap = 0;
	ps3_audio_dropouts = 0;
	ps3_audio_playedFrames = 0;
	ps3_audio_primed = false;

	// Priority 100, above the main thread's 1001 (lv2: 0=highest) -- a hard
	// ~5.3ms deadline per block must preempt, or heavy main-thread load crackles.
	{
		static char audio_thread_name[] = "PS3AudioThread";

		ret = sysThreadCreate( &ps3_audio_thread, PS3_Audio_ThreadFunc, NULL,
			100, 0x4000, THREAD_JOINABLE, audio_thread_name );
	}
	if( ret != 0 )
	{
		Con_Printf( "SNDDMA_Init: sysThreadCreate failed: 0x%08x\n", (unsigned int)ret );
		ps3_audio_running = false;
		snd.initialized = false;
		Mem_Free( snd.buffer );
		snd.buffer = NULL;
		audioPortStop( ps3_audio_port );
		audioRemoveNotifyEventQueue( ps3_audio_queueKey );
		sysEventQueueDestroy( ps3_audio_eventQ, 0 );
		audioPortClose( ps3_audio_port );
		audioQuit();
		return false;
	}

	Con_Printf( "SNDDMA_Init: OK - %d Hz, 16-bit, %d ch\n", PS3_AUDIO_RATE, PS3_AUDIO_CHANNELS );

	return true;
}

void SNDDMA_BeginPainting( void )
{
	// Main thread only -- Con_Printf isn't safe from the audio thread and would
	// itself blow the 5.33ms deadline. Throttled to a rate, not a flood.
	static double lastReport;
	static u32    lastSilent, lastDropouts;
	double        now;
	u32           silent, silentFrames, dropouts;

	if( !ps3_audio_running )
		return;

	silent       = ps3_audio_silentBlocks;
	silentFrames = ps3_audio_silentFrames;
	dropouts     = ps3_audio_dropouts;

	if( silent == lastSilent && dropouts == lastDropouts )
		return;

	now = Platform_DoubleTime();

	if( lastReport != 0.0 && ( now - lastReport ) < 1.0 )
		return;

	// Reported on the MAIN thread, so a burst during a blocking load logs after
	// the stall it describes, not during it.
	Con_Printf( S_WARN "PS3_Audio: starved -- %u blocks padded with silence (+%u), %.0fms total, longest gap %.0fms, %u resyncs (+%u)\n",
		silent, silent - lastSilent,
		silentFrames * 1000.0 / PS3_AUDIO_RATE,
		ps3_audio_longestGap * 1000.0 / PS3_AUDIO_RATE,
		dropouts, dropouts - lastDropouts );

	lastReport   = now;
	lastSilent   = silent;
	lastDropouts = dropouts;
}

void SNDDMA_Submit( void )
{
}

void SNDDMA_Shutdown( void )
{
	PS3_DIAG( "SNDDMA_Shutdown: enter" );

	if( ps3_audio_running )
	{
		u64 retval;

		ps3_audio_quit = 1;
		ps3_audio_running = false;

		PS3_DIAG( "SNDDMA_Shutdown: before sysThreadJoin" );
		sysThreadJoin( ps3_audio_thread, &retval );
		PS3_DIAG( "SNDDMA_Shutdown: after sysThreadJoin" );

		audioPortStop( ps3_audio_port );
		audioRemoveNotifyEventQueue( ps3_audio_queueKey );
		sysEventQueueDestroy( ps3_audio_eventQ, 0 );
		audioPortClose( ps3_audio_port );
		audioQuit();

		PS3_DIAG( "SNDDMA_Shutdown: after audioQuit" );
	}

	snd.initialized = false;

	if( snd.buffer )
	{
		Mem_Free( snd.buffer );
		snd.buffer = NULL;
	}

	PS3_DIAG( "SNDDMA_Shutdown: exit" );
}

void SNDDMA_Activate( qboolean active )
{
}

qboolean VoiceCapture_Init( void )
{
	return false;
}

void VoiceCapture_Shutdown( void )
{
}

qboolean VoiceCapture_Activate( qboolean activate )
{
	return false;
}

qboolean VoiceCapture_Lock( qboolean lock )
{
	return false;
}
