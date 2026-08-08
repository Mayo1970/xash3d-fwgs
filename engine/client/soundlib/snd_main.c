/*
snd_main.c - load & save various sound formats
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "soundlib.h"
#if XASH_PS3
#include "platform/platform.h"
#endif

static void Sound_Reset( void )
{
	// reset global variables
	sound.width = sound.rate = 0;
	sound.channels = sound.loopstart = 0;
	sound.samples = sound.flags = 0;
	sound.type = WF_UNKNOWN;

	sound.wav = NULL;
	sound.size = 0;
}

static MALLOC_LIKE( FS_FreeSound, 1 ) wavdata_t *SoundPack( void )
{
	wavdata_t *pack = Mem_Malloc( host.soundpool, sizeof( *pack ) + sound.size );

	pack->size = sound.size;
	pack->loop_start = sound.loopstart;
	pack->samples = sound.samples;
	pack->type = sound.type;
	pack->flags = sound.flags;
	pack->rate = sound.rate;
	pack->width = sound.width;
	pack->channels = sound.channels;
	memcpy( pack->buffer, sound.wav, sound.size );

	Mem_Free( sound.wav );
	sound.wav = NULL;

	return pack;
}

/*
================
FS_LoadSound

loading and unpack to wav any known sound
================
*/
wavdata_t *FS_LoadSound( const char *filename, const byte *buffer, size_t size )
{
	const char *ext = COM_FileExtension( filename );
	string loadname;
	qboolean anyformat = true;
#if XASH_PS3
	// Declared up here, ahead of the "goto load_internal" below, because
	// -Werror=jump-misses-init rejects a jump that skips an initialization.
	double  ps3_load_start = Platform_DoubleTime();
	double  ps3_io_ms = 0.0;
	int     ps3_attempts = 0;
#endif

	Sound_Reset(); // clear old sounddata
	Q_strncpy( loadname, filename, sizeof( loadname ));

	if( !COM_StringEmpty( ext ))
	{
		// we needs to compare file extension with list of supported formats
		// and be sure what is real extension, not a filename with dot
		for( const loadwavfmt_t *format = sound.loadformats; format && format->ext; format++ )
		{
			if( !Q_stricmp( format->ext, ext ))
			{
				COM_StripExtension( loadname );
				anyformat = false;
				break;
			}
		}
	}

	// special mode: skip any checks, load file from buffer
	if( filename[0] == '#' && buffer && size )
		goto load_internal;

	// A name carrying no recognised extension leaves anyformat true, so the
	// loop below probes EVERY entry in sound.loadformats (wav, mp3, ogg, opus)
	// and each miss is a full searchpath walk over LV2's high-per-call-latency
	// filesystem. A name that does carry one costs a single lookup. Those two
	// cases want opposite fixes -- preloading the file vs. never probing for
	// formats this port does not ship -- and the existing "late precache took
	// Nms" line in S_LoadSound cannot tell them apart, so the PS3 blocks below
	// record which extension actually won and how many probes it took.
	//
	// Only reported past ~110ms, which is this port's real mixer cushion
	// (_snd_mixahead 0.12 * SOUND_DMA_SPEED 44100 = 5292 frames, against
	// 48000Hz output). Below that a load cannot starve the feeder thread in
	// engine/platform/ps3/s_ps3.c, so it is not worth a log line.

	// now try all the formats in the selected list
	for( const loadwavfmt_t *format = sound.loadformats; format && format->ext; format++)
	{
		if( anyformat || !Q_stricmp( ext, format->ext ))
		{
			qboolean success = false;
			fs_offset_t filesize = 0;
			string path;

#if XASH_PS3
			// Callers use BOTH conventions: most pass a name relative to
			// DEFAULT_SOUNDPATH ("common/foo.wav"), but some -- mainui's menu
			// sounds among them -- pass one that already carries the prefix
			// ("sound/common/foo.wav"). S_SoundList_f (s_load.c) branches on
			// exactly this distinction, so it is an expected shape, not a
			// caller bug. Prefixing unconditionally turns the second kind
			// into "sound/sound/common/foo.wav", which can never resolve.
			//
			// Stock code papered over that with a second bare-path probe
			// after the first miss; the PS3 build compiles that fallback out
			// (below) to avoid doubling LV2 round-trips, which silently broke
			// every already-prefixed name -- they fell through to
			// S_CreateDefaultSound after burning a full searchpath walk.
			// Hardware caught it on sound/common/launch_select2.wav: MISSED
			// all 1 probe(s), 331.66ms wasted.
			//
			// Choosing the prefix up front is strictly better than either:
			// one lookup instead of two for the common case, and a correct
			// lookup instead of a guaranteed miss for the prefixed case.
			Q_snprintf( path, sizeof( path ), "%s%s.%s",
				Q_strnicmp( loadname, DEFAULT_SOUNDPATH, sizeof( DEFAULT_SOUNDPATH ) - 1 ) ? DEFAULT_SOUNDPATH : "",
				loadname, format->ext );
#else
			Q_snprintf( path, sizeof( path ), DEFAULT_SOUNDPATH "%s.%s", loadname, format->ext );
#endif

#if XASH_PS3
			ps3_attempts++;
			double ps3_io_start = Platform_DoubleTime();
#endif

			byte *f = FS_LoadFile( path, &filesize, false );

#if XASH_PS3
			// Charged per probe, so a miss walk accumulates its wasted
			// searchpath time here too. Everything outside this window is
			// decode: the WAV parse, the big-endian per-sample swap loop
			// (snd_wav.c), and SoundPack's second full copy of the buffer.
			// Splitting them is what tells a slow LV2 filesystem apart from
			// a slow PPE-side decode -- the swap loop is the one stage that
			// costs nothing on a little-endian host, so if endianness were
			// driving these load times it would show up as decode, not I/O.
			ps3_io_ms += ( Platform_DoubleTime() - ps3_io_start ) * 1000.0;
#endif

			if( f && filesize > 0 )
			{
				success = format->loadfunc( path, f, filesize );
				Mem_Free( f ); // release buffer
			}

			if( success )
			{
#if XASH_PS3
				double total_ms = ( Platform_DoubleTime() - ps3_load_start ) * 1000.0;

				if( total_ms > 110.0 )
					Con_Printf( S_WARN "%s: \"%s\" hit on .%s after %d probe(s), %.2fms total = %.2f io + %.2f decode (%s)\n",
						__func__, loadname, format->ext, ps3_attempts, total_ms,
						ps3_io_ms, total_ms - ps3_io_ms,
						anyformat ? "no extension in name -- probed blind" : "extension given" );
#endif
				return SoundPack(); // loaded
			}

#if !XASH_PS3
			// Every real sound asset in this codebase is authored under
			// DEFAULT_SOUNDPATH ("sound/") -- every call site that builds a
			// sound path uses that prefix (see cl_tent.c, sv_init.c, s_vox.c's
			// sentences.txt lookup, etc.). This bare-name fallback exists for
			// callers that already pass a fully qualified path without the
			// prefix, which is a real but rare case worth keeping on other
			// platforms. On PS3, LV2 filesystem calls have high per-call
			// latency (see the PS3 skill's own anti-pattern notes), and a
			// miss on an unprefixed VOX/sfx name -- which anyformat-mode
			// hits for every format in sound.loadformats -- doubles the
			// wasted round-trips for no benefit on this port's actual asset
			// layout. Skipping it here just means a name that only resolves
			// via the bare-path fallback will fail to load on PS3; nothing
			// in this port's shipped or expected mod content relies on that.
			Q_snprintf( path, sizeof( path ), "%s.%s", loadname, format->ext );
			f = FS_LoadFile( path, &filesize, false );
			if( f && filesize > 0 )
			{
				success = format->loadfunc( path, f, filesize );
				Mem_Free( f ); // release buffer
			}

			if( success )
				return SoundPack();
#endif
		}
	}

#if XASH_PS3
	// Fell out of the loop with nothing loaded: every probe was a full
	// searchpath walk that returned nothing, and S_LoadSound will now
	// substitute S_CreateDefaultSound. This is the most expensive shape this
	// function has -- all of the latency, none of the sound -- and it is
	// invisible in the existing "late precache took Nms" line, which reports
	// the same duration whether the file was found or not.
	{
		double total_ms = ( Platform_DoubleTime() - ps3_load_start ) * 1000.0;

		if( ps3_attempts > 0 && total_ms > 110.0 )
			Con_Printf( S_WARN "%s: \"%s\" MISSED all %d probe(s), %.2fms wasted (%.2f io) (%s)\n",
				__func__, loadname, ps3_attempts, total_ms, ps3_io_ms,
				anyformat ? "no extension in name -- probed blind" : "extension given" );
	}
#endif

load_internal:
	for( const loadwavfmt_t *format = sound.loadformats; format && format->ext; format++ )
	{
		if( anyformat || !Q_stricmp( ext, format->ext ))
		{
			if( buffer && size > 0  )
			{
				if( format->loadfunc( loadname, buffer, size ))
					return SoundPack(); // loaded
			}
		}
	}

	if( filename[0] != '#' )
		Con_DPrintf( S_WARN "%s: couldn't load \"%s\"\n", __func__, loadname );

	return NULL;
}

/*
================
Sound_FreeSound

free WAV buffer
================
*/
void FS_FreeSound( wavdata_t *pack )
{
	if( !pack ) return;
	Mem_Free( pack );
}

/*
================
FS_OpenStream

open and reading basic info from sound stream
================
*/
stream_t *FS_OpenStream( const char *filename )
{
	const char	*ext = COM_FileExtension( filename );
	string		loadname;
	qboolean		anyformat = true;
	stream_t		*stream = NULL;

	Sound_Reset(); // clear old streaminfo
	Q_strncpy( loadname, filename, sizeof( loadname ));

	if( !COM_StringEmpty( ext ))
	{
		// we needs to compare file extension with list of supported formats
		// and be sure what is real extension, not a filename with dot
		for( const streamfmt_t *format = sound.streamformat; format && format->ext; format++ )
		{
			if( !Q_stricmp( format->ext, ext ))
			{
				COM_StripExtension( loadname );
				anyformat = false;
				break;
			}
		}
	}

	// now try all the formats in the selected list
	for( const streamfmt_t *format = sound.streamformat; format && format->ext; format++)
	{
		if( anyformat || !Q_stricmp( ext, format->ext ))
		{
			string path;

			Q_snprintf( path, sizeof( path ), "%s.%s", loadname, format->ext );

			if(( stream = format->openfunc( path )) != NULL )
			{
				stream->format = format;
				return stream; // done
			}
		}
	}

	// compatibility with original Xash3D, try media/ folder
	if( Q_strncmp( filename, "media/", sizeof( "media/" ) - 1 ))
	{
		Q_snprintf( loadname, sizeof( loadname ), "media/%s", filename );
		stream = FS_OpenStream( loadname );
	}
	else
	{
		Con_Reportf( "%s: couldn't open \"%s\" or \"%s\"\n", __func__, filename + 6, filename );
	}

	return stream;
}

/*
================
FS_ReadStream

extract stream as wav-data and put into buffer, move file pointer
================
*/
int FS_ReadStream( stream_t *stream, int bytes, void *buffer )
{
	if( !stream || !stream->format || !stream->format->readfunc )
		return 0;

	if( bytes <= 0 || buffer == NULL )
		return 0;

	return stream->format->readfunc( stream, bytes, buffer );
}

/*
================
FS_GetStreamPos

get stream position (in bytes)
================
*/
int FS_GetStreamPos( stream_t *stream )
{
	if( !stream || !stream->format || !stream->format->getposfunc )
		return -1;

	return stream->format->getposfunc( stream );
}

/*
================
FS_SetStreamPos

set stream position (in bytes)
================
*/
int FS_SetStreamPos( stream_t *stream, int newpos )
{
	if( !stream || !stream->format || !stream->format->setposfunc )
		return -1;

	return stream->format->setposfunc( stream, newpos );
}

/*
================
FS_FreeStream

close sound stream
================
*/
void FS_FreeStream( stream_t *stream )
{
	if( !stream || !stream->format || !stream->format->freefunc )
		return;

	stream->format->freefunc( stream );
}

#if XASH_LLVM_LIBFUZZER
#define IMPLEMENT_SOUNDLIB_FUZZ_TARGET( export, target ) \
int EXPORT export( const uint8_t *Data, size_t Size ); \
int EXPORT export( const uint8_t *Data, size_t Size ) \
{ \
	wavdata_t *wav; \
	host.type = HOST_NORMAL; \
	Memory_Init(); \
	Sound_Init(); \
	if( target( "#internal", Data, Size )) \
	{ \
		wav = SoundPack(); \
		FS_FreeSound( wav ); \
	} \
	Sound_Shutdown(); \
	return 0; \
} \

IMPLEMENT_SOUNDLIB_FUZZ_TARGET( Fuzz_Sound_LoadMPG, Sound_LoadMPG )
IMPLEMENT_SOUNDLIB_FUZZ_TARGET( Fuzz_Sound_LoadWAV, Sound_LoadWAV )
#endif // XASH_LLVM_LIBFUZZER
