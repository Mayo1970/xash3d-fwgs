/*
s_load.c - sounds managment
Copyright (C) 2007 Uncle Mike

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
#include "client.h"
#include "sound.h"
#if XASH_PS3
#include "platform/platform.h" // PS3_ProbeMemory
#include "ps3_sound_preload.h"
#endif

// during registration it is possible to have more sounds
// than could actually be referenced during gameplay,
// because we don't want to free anything until we are
// sure we won't need it.
#define MAX_SFX      8192
#define MAX_SFX_HASH (MAX_SFX/4)

static int      s_numSfx = 0;
static sfx_t    s_knownSfx[MAX_SFX];
static sfx_t    *s_sfxHashList[MAX_SFX_HASH];
static qboolean s_registering = false;

#define SENTENCE_INDEX -99999 // unique sentence index
static string   s_sentenceImmediateName;	// keep dummy sentence name

#if XASH_PS3
static void S_RegisterClientEffectSounds( void )
{
	int registered = 0;
	int requested = 0;

	// Client temp entities choose from these groups at runtime. Register every
	// variant now so S_EndRegistration does the disk I/O during map loading.
	for( int group = BouncePlayerShell; group <= Explode; group++ )
	{
		soundlst_group_t sound_group = (soundlst_group_t)group;
		int count = SoundList_Count( sound_group );
		requested += count;

		for( int i = 0; i < count; i++ )
		{
			const char *name = SoundList_Get( sound_group, i );

			if( name && S_RegisterSound( name ) >= 0 )
				registered++;
		}
	}

	Con_Printf( "PS3_Audio: registered %d/%d client effect sounds for map preload\n",
		registered, requested );
}

// The mod's client DLL plays these by name during gameplay (weapon event
// scripts, HUD, temp entities) and the server precaches only some of them, so
// a first use blocks the main thread in S_LoadSound past the ~110ms cushion.
// Table is generated from the vendored client sources -- see
// scripts/ps3_gen_sound_preload.py.
static void S_RegisterModClientSounds( void )
{
	const ps3_sound_preload_t *tbl = NULL;
	int registered = 0;
	int absent = 0;

	for( int i = 0; i < (int)ARRAYSIZE( ps3_sound_preload ); i++ )
	{
		if( !Q_stricmp( GI->gamefolder, ps3_sound_preload[i].gamedir ))
		{
			tbl = &ps3_sound_preload[i];
			break;
		}
	}

	if( !tbl )
		return;

	for( int i = 0; i < tbl->count; i++ )
	{
		// A name with no file on disk caches a 1-second default sound instead
		// (88Kb each), so skip it rather than pay memory for a guaranteed miss.
		if( !FS_FileExists( va( DEFAULT_SOUNDPATH "%s", tbl->names[i] ), false ))
		{
			absent++;
			continue;
		}

		if( S_RegisterSound( tbl->names[i] ) >= 0 )
			registered++;
	}

	Con_Printf( "PS3_Audio: registered %d/%d %s client sounds for map preload, %d absent\n",
		registered, tbl->count, tbl->gamedir, absent );
}
#endif

/*
=================
S_SoundList_f
=================
*/
void S_SoundList_f( void )
{
	sfx_t	*sfx;
	int	i, totalSfx = 0;
	int	totalSize = 0;

	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] )
			continue;

		wavdata_t *sc = sfx->cache;
		if( sc )
		{
			totalSize += sc->size;

			if( FBitSet( sc->flags, SOUND_LOOPED ))
				Con_Printf( "L" );
			else
				Con_Printf( " " );

			if( sfx->name[0] == '*' || !Q_strncmp( sfx->name, DEFAULT_SOUNDPATH, sizeof( DEFAULT_SOUNDPATH ) - 1 ))
				Con_Printf( " (%2db) %s : %s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			else Con_Printf( " (%2db) %s : " DEFAULT_SOUNDPATH "%s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			totalSfx++;
		}
	}

	Con_Printf( "-------------------------------------------\n" );
	Con_Printf( "%i total sounds\n", totalSfx );
	Con_Printf( "%s total memory\n", Q_memprint( totalSize ));
	Con_Printf( "\n" );
}

/*
=================
S_CreateDefaultSound
=================
*/
static wavdata_t *S_CreateDefaultSound( void )
{
	uint samples = SOUND_DMA_SPEED;
	uint channels = 1;
	uint width = 2;
	size_t size = samples * width * channels;

	wavdata_t *sc = Mem_Calloc( sndpool, sizeof( wavdata_t ) + size );

	sc->width = width;
	sc->channels = channels;
	sc->rate = SOUND_DMA_SPEED;
	sc->samples = samples;
	sc->size = size;

	return sc;
}

/*
=================
S_LoadSound
=================
*/
wavdata_t *S_LoadSound( sfx_t *sfx )
{
	wavdata_t	*sc = NULL;

	if( !sfx ) return NULL;

	// see if still in memory
	if( sfx->cache )
		return sfx->cache;

	if( COM_StringEmptyOrNULL( sfx->name ))
		return NULL;

	// load it from disk
	if( Q_stricmp( sfx->name, "*default" ))
	{
		qboolean warn_late = ( s_warn_late_precache.value > 0 && cls.state == ca_active );
#if XASH_PS3
		double load_start = warn_late ? Platform_DoubleTime() : 0.0;
#endif

		// load it from disk
		if( warn_late )
			Con_Printf( S_WARN "%s: late precache of %s\n", __func__, sfx->name );

		if( sfx->name[0] == '*' )
			sc = FS_LoadSound( sfx->name + 1, NULL, 0 );
		else
			sc = FS_LoadSound( sfx->name, NULL, 0 );

#if XASH_PS3
		// Confirms the stall is really inside FS_LoadSound's disk I/O, not
		// something else that happens to correlate with the same scripted
		// trigger (e.g. the door/event entity's own server-side logic) --
		// see the late-precache warning above for the "what", this is the
		// "how long", printed together so a hardware log line answers both.
		if( warn_late )
			Con_Printf( S_WARN "%s: late precache of %s took %.2fms\n",
				__func__, sfx->name, ( Platform_DoubleTime() - load_start ) * 1000.0 );
#endif
	}

	if( !sc ) sc = S_CreateDefaultSound();

#if XASH_PS3
	// The resample below sits AFTER the timing print above, so none of it has
	// ever appeared in a "late precache took Nms" line -- a sound whose rate
	// is not 11k/22k/44k pays a full-buffer resample that the existing
	// instrumentation reports as zero. Time it separately rather than folding
	// it into the load figure, because the fixes differ: a slow load wants
	// preloading, a slow resample wants the asset re-cooked to a native rate.
	{
		double resample_start = Platform_DoubleTime();
		word   rate_before = sc->rate;
#endif

	if( sc->rate < SOUND_11k ) // some bad sounds
		Sound_Process( &sc, SOUND_11k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_11k && sc->rate < SOUND_22k ) // some bad sounds
		Sound_Process( &sc, SOUND_22k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_22k && sc->rate != SOUND_44k ) // some bad sounds
		Sound_Process( &sc, SOUND_44k, sc->width, sc->channels, SOUND_RESAMPLE );

#if XASH_PS3
		double resample_ms = ( Platform_DoubleTime() - resample_start ) * 1000.0;

		if( resample_ms > 10.0 )
			Con_Printf( S_WARN "%s: %s RESAMPLED %d -> %d Hz, %.2fms\n",
				__func__, sfx->name, (int)rate_before, (int)sc->rate, resample_ms );
	}
#endif

	sfx->cache = sc;

	return sfx->cache;
}

/*
==================
S_FindName

==================
*/
sfx_t *S_FindName( const char *pname, qboolean *pfInCache )
{
	sfx_t	*sfx;
	uint	i;
	string	name;

	if( COM_StringEmptyOrNULL( pname ) || !snd.initialized )
		return NULL;

	if( Q_strlen( pname ) >= sizeof( sfx->name ))
		return NULL;

	Q_strncpy( name, pname, sizeof( name ));
	COM_FixSlashes( name );

	// see if already loaded
	uint hash = COM_HashKey( name, MAX_SFX_HASH );
	for( sfx = s_sfxHashList[hash]; sfx; sfx = sfx->hashNext )
	{
		if( !Q_strcmp( sfx->name, name ))
		{
			if( pfInCache )
			{
				// indicate whether or not sound is currently in the cache.
				*pfInCache = ( sfx->cache != NULL ) ? true : false;
			}
			// prolonge registration
			sfx->servercount = cl.servercount;
			return sfx;
		}
	}

	// find a free sfx slot spot
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++)
		if( !sfx->name[0] ) break; // free spot

	if( i == s_numSfx )
	{
		if( s_numSfx == MAX_SFX )
			return NULL;
		s_numSfx++;
	}

	sfx = &s_knownSfx[i];
	memset( sfx, 0, sizeof( *sfx ));
	if( pfInCache ) *pfInCache = false;
	Q_strncpy( sfx->name, name, sizeof( sfx->name ));
	sfx->servercount = cl.servercount;
	sfx->hashValue = COM_HashKey( sfx->name, MAX_SFX_HASH );

	// link it in
	sfx->hashNext = s_sfxHashList[sfx->hashValue];
	s_sfxHashList[sfx->hashValue] = sfx;

	return sfx;
}

/*
==================
S_FreeSound
==================
*/
void S_FreeSound( sfx_t *sfx )
{
	if( !sfx || !sfx->name[0] )
		return;

	// de-link it from the hash tree
	sfx_t **prev = &s_sfxHashList[sfx->hashValue];
	while( 1 )
	{
		sfx_t *hashSfx = *prev;
		if( !hashSfx )
			break;

		if( hashSfx == sfx )
		{
			*prev = hashSfx->hashNext;
			break;
		}
		prev = &hashSfx->hashNext;
	}

	if( clgame.soundFuncs.pfnS_FreeSound )
	{
		clgame.soundFuncs.pfnS_FreeSound( sfx, sfx - s_knownSfx );
		return;
	}

	if( sfx->cache )
		FS_FreeSound( sfx->cache );
	memset( sfx, 0, sizeof( *sfx ));
}

/*
=====================
S_BeginRegistration

=====================
*/
void S_BeginRegistration( void )
{
	snd.have_ambient_sfx = false;

#if XASH_PS3
	// skip on multiplayer-only games (e.g. CS): they never speak vox words,
	// and preloading CS's inherited HL1 sentences.txt cost 13.79Mb for nothing
	if( GI->gamemode != GAME_MULTIPLAYER_ONLY )
	{
		PS3_ProbeMemory( "before VOX preload" );
		VOX_PreloadDeferred();
		PS3_ProbeMemory( "after VOX preload" );
	}
	else Con_Printf( "VOX preload skipped: %s is multiplayer-only\n", GI->gamefolder );
#endif

	// check for automatic ambient sounds
	for( int i = 0; i < NUM_AMBIENTS; i++ )
	{
		if( !GI->ambientsound[i][0] )
			continue;	// empty slot

		snd.ambient_sfx[i] = S_RegisterSound( GI->ambientsound[i] );
		if( snd.ambient_sfx[i] )
			snd.have_ambient_sfx = true; // allow auto-ambients
	}

	s_registering = true;

#if XASH_PS3
	S_RegisterClientEffectSounds();
	S_RegisterModClientSounds();
#endif
}

/*
=====================
S_EndRegistration

=====================
*/
void S_EndRegistration( void )
{
	sfx_t	*sfx;
	int	i;

	if( !s_registering || !snd.initialized )
		return;

	// free any sounds not from this registration sequence
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] || !Q_stricmp( sfx->name, "*default" ))
			continue; // don't release default sound

		if( sfx->servercount != cl.servercount )
			S_FreeSound( sfx ); // don't need this sound
	}

	// load everything in
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] )
			continue;
		S_LoadSound( sfx );
	}
	s_registering = false;
}

/*
==================
S_RegisterSound

==================
*/
sound_t S_RegisterSound( const char *name )
{
	sfx_t	*sfx;

	if( COM_StringEmptyOrNULL( name ) || !snd.initialized )
		return -1;

	if( S_TestSoundChar( name, '!' ))
	{
		Q_strncpy( s_sentenceImmediateName, name, sizeof( s_sentenceImmediateName ));
		return SENTENCE_INDEX;
	}

	// some stupid mappers used leading '/' or '\' in path to models or sounds
	if( name[0] == '/' || name[0] == '\\' ) name++;
	if( name[0] == '/' || name[0] == '\\' ) name++;

	sfx = S_FindName( name, NULL );
	if( !sfx ) return -1;

	sfx->servercount = cl.servercount;
	if( !s_registering ) S_LoadSound( sfx );

	return sfx - s_knownSfx;
}

sfx_t *S_GetSfxByHandle( sound_t handle )
{
	if( !snd.initialized )
		return NULL;

	// create new sfx
	if( handle == SENTENCE_INDEX )
		return S_FindName( s_sentenceImmediateName, NULL );

	if( handle < 0 || handle >= s_numSfx )
		return NULL;

	return &s_knownSfx[handle];
}

/*
=================
S_InitSounds
=================
*/
void S_InitSounds( void )
{
	// create unused 0-entry
	Q_strncpy( s_knownSfx->name, "*default", sizeof( s_knownSfx->name ));
	s_knownSfx->hashValue = COM_HashKey( s_knownSfx->name, MAX_SFX_HASH );
	s_knownSfx->hashNext = s_sfxHashList[s_knownSfx->hashValue];
	s_sfxHashList[s_knownSfx->hashValue] = s_knownSfx;
	s_knownSfx->cache = S_CreateDefaultSound();
	s_numSfx = 1;
}

/*
=================
S_FreeSounds
=================
*/
void S_FreeSounds( void )
{
	sfx_t	*sfx;
	int	i;

	if( !snd.initialized )
		return;

	// stop all sounds
	S_StopAllSounds( true );

	// free all sounds
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
		S_FreeSound( sfx );


	memset( s_knownSfx, 0, sizeof( s_knownSfx ));
	memset( s_sfxHashList, 0, sizeof( s_sfxHashList ));

	s_numSfx = 0;
}
