/*
s_vox.c - npc sentences
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

#include "common.h"
#include "sound.h"
#include "const.h"
#if XASH_PS3
#include "client.h" // SCR_BootProgress
#endif
#include <ctype.h>

#define TRIM_SCAN_MAX 255
#define TRIM_SAMPLES_BELOW_8 2
#define TRIM_SAMPLES_BELOW_16 512 // 65k * 2 / 256

#define CVOXFILESENTENCEMAX 4096

static int cszrawsentences = 0;
static char *rgpszrawsentence[CVOXFILESENTENCEMAX];
static const char *voxperiod = "_period", *voxcomma = "_comma";

static qboolean S_ShouldTrimSample8( const int8_t *buf, int channels )
{
	if( abs( buf[0] ) > TRIM_SAMPLES_BELOW_8 )
		return false;

	if( channels >= 2 && abs( buf[1] ) > TRIM_SAMPLES_BELOW_8 )
		return false;

	return true;
}

static qboolean S_ShouldTrimSample16( const int16_t *buf, int channels )
{
	if( abs( buf[0] ) > TRIM_SAMPLES_BELOW_16 )
		return false;

	if( channels >= 2 && abs( buf[1] ) > TRIM_SAMPLES_BELOW_16 )
		return false;

	return true;
}

static int S_TrimStart( const wavdata_t *wav, int start )
{
	size_t channels = wav->channels, width = wav->width;

	if( wav->type != WF_PCMDATA )
		return start;

	if( width == 1 )
	{
		const int8_t *data = (const int8_t *)&wav->buffer[channels * width * start];

		for( size_t i = 0; i < TRIM_SCAN_MAX && start < wav->samples; i++ )
		{
			if( !S_ShouldTrimSample8( data, wav->channels ))
				break;

			start += channels;
			data += channels;
		}
	}
	else if( width == 2 )
	{
		const int16_t *data = (const int16_t *)&wav->buffer[channels * width * start];

		for( size_t i = 0; i < TRIM_SCAN_MAX && start < wav->samples; i++ )
		{
			if( !S_ShouldTrimSample16( data, wav->channels ))
				break;

			start += channels;
			data += channels;
		}
	}

	return start;
}

static int S_TrimEnd( const wavdata_t *wav, int end )
{
	size_t channels = wav->channels, width = wav->width;

	if( wav->type != WF_PCMDATA )
		return end;

	if( width == 1 )
	{
		const int8_t *data = (const int8_t *)&wav->buffer[channels * width * ( end - 1 )];

		for( size_t i = 0; i < TRIM_SCAN_MAX && end > 0; i++ )
		{
			if( !S_ShouldTrimSample8( data, wav->channels ))
				break;

			end -= channels;
			data -= channels;
		}
	}
	else if( width == 2 )
	{
		const int16_t *data = (const int16_t *)&wav->buffer[channels * width * ( end - 1 )];

		for( size_t i = 0; i < TRIM_SCAN_MAX && end > 0; i++ )
		{
			if( !S_ShouldTrimSample16( data, wav->channels ))
				break;

			end -= channels;
			data -= channels;
		}
	}

	return end;
}

static void S_TrimStartEndTimes( channel_t *ch, wavdata_t *wav, int start, int end )
{
	ch->sample = start = S_TrimStart( wav, start );

	// don't overrun the buffer while trimming end
	if( end == 0 )
		end = wav->samples - wav->channels;

	if( end < start )
		end = start;

	ch->forced_end = S_TrimEnd( wav, end );
}

void VOX_LoadWord( channel_t *ch )
{
	SetBits( ch->flags, FL_CHAN_SENTENCE_FINISHED );

	if( ch->word_index < 0 || ch->word_index >= CVOXWORDMAX )
		return;

	const voxword_t *word = &ch->words[ch->word_index];

	if( !word->sfx )
		return;

	wavdata_t *data = S_LoadSound( word->sfx );

	if( !data )
		return;

	ClearBits( ch->flags, FL_CHAN_SENTENCE_FINISHED );
	ch->data = data;

	int start = word->start;
	int end   = word->end;

	if( end <= start )
		end = 0;

	S_TrimStartEndTimes( ch, data, round( start * 0.01f * data->samples ), round( end * 0.01f * data->samples ));
}

void VOX_FreeWord( channel_t *ch )
{
	// TODO: don't set random fields to zero lol, was memset before
	ch->sample = ch->forced_end = 0.0;
	ClearBits( ch->flags, FL_CHAN_FINISHED );
	ch->data = NULL;

	if( ch->word_index < 0 || ch->word_index >= CVOXWORDMAX )
		return;

	voxword_t *word = &ch->words[ch->word_index];

	if( !word->sfx )
		return;

#if !XASH_PS3
	// Purging every word's decoded cache as soon as it finishes playing was a
	// memory optimization for 16MB-class machines; sentence words are small
	// (a few KB of 11kHz mono) and HL1 replays the same fvox/scientist/etc.
	// word pool constantly, so on a platform with tens of MB free this just
	// guarantees the next playback re-hits synchronous main-thread disk I/O
	// (see engine/client/sound/s_load.c's S_LoadSound -> FS_LoadFile) for a
	// word that will be needed again shortly. Confirmed via real hardware
	// telemetry: repeated NPC dialogue lines (e.g. scientist intro sequence
	// at c0a0) never let the frame-time window recover between sentences,
	// because every word is a fresh disk load every single time. PS3 keeps
	// the cache populated instead -- sfx_t entries already persist for the
	// whole session regardless (see S_FindName/S_FreeSound), so this only
	// changes whether ->cache is warm, not any lifetime/ownership rule.
	if( !FBitSet( word->flags, FL_VOXWORD_IN_CACHE ))
	{
		FS_FreeSound( word->sfx->cache );
		word->sfx->cache = NULL;
	}
#endif

	word->sfx = NULL;
}

void VOX_SetChanVol( channel_t *ch )
{
	voxword_t *word;

	if( !ch->words || FBitSet( ch->flags, FL_CHAN_SENTENCE_FINISHED ))
		return;

	word = &ch->words[ch->word_index];

	if( word->volume == 100 )
		return;

	ch->leftvol = ch->leftvol * word->volume * 0.01f;
	ch->rightvol = ch->rightvol * word->volume * 0.01f;
}

float VOX_ModifyPitch( channel_t *ch, float pitch )
{
	voxword_t *word;

	if( !ch->words || FBitSet( ch->flags, FL_CHAN_SENTENCE_FINISHED ))
		return pitch;

	word = &ch->words[ch->word_index];

	if( word->pitch == PITCH_NORM )
		return pitch;

	pitch += ( word->pitch - PITCH_NORM ) * 0.01f;

	return pitch;
}

static const char *VOX_GetDirectory( char *szpath, const char *psz, int nsize )
{
	// HACKHACK: some modders send strings like "/fvox/_period four"
	// which should get parsed as "_period four" said by fvox
	// it might be incorrect but ignore first slash here for now
	if( psz[0] == '/' )
		psz++;

	// search / backwards
	const char *p = Q_strrchr( psz, '/' );

	if( !p )
	{
		Q_strncpy( szpath, "vox/", nsize );
		return psz;
	}

	int len = p - psz + 1;

	if( len > nsize )
	{
		Con_Printf( "%s: invalid directory in: %s\n", __func__, psz );
		return NULL;
	}

	memcpy( szpath, psz, len );
	szpath[len] = 0;

	return p + 1;
}

static const char *VOX_LookupString( const char *pszin )
{
	int i = -1;

	// check if we are an immediate sentence
	if( *pszin == '#' )
	{
		// immediate sentence, probably coming from "speak" command
		return pszin + 1;
	}

	// check if we received an index
	if( Q_isdigit( pszin ))
	{
		i = Q_atoi( pszin );

		if( i >= cszrawsentences )
			i = -1;
	}

	// last hope: find it in sentences array
	if( i == -1 )
	{
		for( i = 0; i < cszrawsentences; i++ )
		{
			if( !Q_stricmp( pszin, rgpszrawsentence[i] ))
				break;
		}
	}

	// not found, exit
	if( i == cszrawsentences )
		return NULL;

	int len = Q_strlen( rgpszrawsentence[i] );

	const char *c = &rgpszrawsentence[i][len + 1];
	for( ; *c == ' ' || *c == '\t'; c++ );

	return c;
}

static int VOX_ParseString( char *psz, char *rgpparseword[CVOXWORDMAX] )
{
	int i = 0;

	if( !psz )
		return i;

	rgpparseword[i++] = psz;

	while( i < CVOXWORDMAX )
	{
		// skip to next word
		//
		// '\t' belongs in this set alongside ' ': sentences.txt is whitespace-
		// separated and nothing distinguishes a tab from a space there. Without
		// it, a body that aligns words with a tab (" \t ") yields a token that
		// is literally "\t", confirmed on hardware by the escaped-token log in
		// VOX_WordTokenIsSane. That token then resolves to no file, so
		// S_LoadSound substitutes S_CreateDefaultSound -- a Mem_Calloc of
		// SOUND_DMA_SPEED samples, i.e. a full SECOND of silence spliced into
		// the middle of the spoken sentence -- after paying a ~247ms
		// four-format searchpath miss to discover it.
		for( ; *psz &&
			*psz != ' ' &&
			*psz != '\t' &&
			*psz != '.' &&
			*psz != ',' &&
			*psz != '('; psz++ );

		// skip anything in between ( and )
		if( *psz == '(' )
		{
			for( ; *psz && *psz != ')'; psz++ );
			psz++;
		}

		if( !*psz )
			return i;

		// . and , are special but if not end of string
		if(( *psz == '.' || *psz == ',' ) &&
			psz[1] != '\n' && psz[1] != '\r' && psz[1] != '\0' )
		{
			if( *psz == '.' )
				rgpparseword[i++] = (char *)voxperiod;
			else rgpparseword[i++] = (char *)voxcomma;

			if( i >= CVOXWORDMAX )
				return i;
		}

		*psz++ = 0;

		// same reasoning as the scan loop above -- a run of separators between
		// two words must swallow tabs too, or the next token starts on one
		for( ; *psz && ( *psz == '.' || *psz == ' ' || *psz == '\t' || *psz == ',' );
		     psz++ );

		if( !*psz )
			return i;

		rgpparseword[i++] = psz;
	}

	return i;
}

static void VOX_MakeDefaultWordParams( voxword_t *voxword )
{
	*voxword = (voxword_t) {
		.volume = 100,
		.pitch = 100,
		.end = 100,
	};
}

static qboolean VOX_ParseWordParams( char *psz, voxword_t *pvoxword, voxword_t *default_voxword )
{
	char sznum[8], *pszsave = psz;

	*pvoxword = *default_voxword;

	int len = Q_strlen( psz );

	if( len == 0 )
		return false;

	// no special params
	if( psz[len-1] != ')' )
		return true;

	for( ; *psz != '(' && *psz != ')'; psz++ );

	// invalid syntax
	if( *psz == ')' )
		return false;

	// split filename and params
	*psz++ = '\0';

	for( ;; )
	{
		char command;
		int i;

		// find command
		for( ; *psz &&
			*psz != 'v' &&
			*psz != 'p' &&
			*psz != 's' &&
			*psz != 'e' &&
			*psz != 't'; psz++ )
		{
			if( *psz == ')' )
				break;
		}

		command = *psz++;

		if( !isdigit((byte)*psz ))
			break;

		memset( sznum, 0, sizeof( sznum ));
		for( i = 0; i < sizeof( sznum ) - 1 && isdigit((byte)*psz ); i++, psz++ )
			sznum[i] = *psz;

		i = Q_atoi( sznum );
		switch( command )
		{
		case 'e':
			pvoxword->end = bound( 0, i, 100 );
			break;
		case 'p':
			pvoxword->pitch = bound( 0, i, UINT16_MAX );
			break;
		case 's':
			pvoxword->start = bound( 0, i, 100 );
			break;
		case 't':
			pvoxword->timecompress = bound( 0, i, 100 );
			break;
		case 'v':
			pvoxword->volume = bound( 0, i, UINT16_MAX );
			break;
		}
	}

	// no actual word but new defaults
	if( Q_strlen( pszsave ) == 0 )
	{
		*default_voxword = *pvoxword;
		return false;
	}

	return true;
}

void VOX_LoadSound( channel_t *ch, const char *pszin )
{
	char buffer[512] = { 0 }, szpath[32] = { 0 };
	char *rgpparseword[CVOXWORDMAX] = { 0 };
	const char *psz;
	int j;
	int num_words;
	voxword_t default_voxword;
	voxword_t words_buf[CVOXWORDMAX + 1]; // local scratch: parsed words + null terminator

	if( !pszin )
		return;

	// free any existing words from a previous sentence on this channel
	if( ch->words )
	{
		VOX_FreeWord( ch );
		Mem_Free2( &ch->words );
	}

	psz = VOX_LookupString( pszin );

	if( !psz )
	{
		// sometimes modders remove sentences but entities continue to use them, so it's a warning, not an error
		Con_Printf( S_WARN "%s: no sentence named %s\n", __func__, pszin );
		return;
	}

	psz = VOX_GetDirectory( szpath, psz, sizeof( szpath ));

	if( !psz )
	{
		Con_Printf( S_ERROR "%s: failed getting directory for %s\n", __func__, pszin );
		return;
	}

	if( Q_strlen( psz ) >= sizeof( buffer ) )
	{
		Con_Printf( S_ERROR "%s: sentence is too long %s\n", __func__, psz );
		return;
	}

	Q_strncpy( buffer, psz, sizeof( buffer ));
	num_words = VOX_ParseString( buffer, rgpparseword );

	VOX_MakeDefaultWordParams( &default_voxword );
	memset( words_buf, 0, sizeof( words_buf ));

	j = 0;
	for( int i = 0; i < num_words; i++ )
	{
		char pathbuffer[MAX_SYSPATH];

		if( !VOX_ParseWordParams( rgpparseword[i], &words_buf[j], &default_voxword ))
			continue;

		if( Q_snprintf( pathbuffer, sizeof( pathbuffer ), "%s%s", szpath, rgpparseword[i] ) < 0 )
		{
			Con_Printf( S_ERROR "%s: path to word in sentence %s is too long\n", __func__, pszin );
			return;
		}

		qboolean in_cache = false;
		words_buf[j].sfx = S_FindName( pathbuffer, &in_cache );
		if( in_cache )
			SetBits( words_buf[j].flags, FL_VOXWORD_IN_CACHE );

		j++;
	}

	// words_buf[j].sfx is already NULL from the memset — null terminator
	ch->words = Mem_Malloc( sndpool, ( j + 1 ) * sizeof( voxword_t ));
	memcpy( ch->words, words_buf, ( j + 1 ) * sizeof( voxword_t ));

	ch->sfx = ch->words[0].sfx;
	ch->word_index = 0;
	VOX_LoadWord( ch );
}

static void VOX_ReadSentenceFile_( byte *buf, fs_offset_t size )
{
	char *p, *last;

	p = (char *)buf;
	last = p + size;

	while( p < last )
	{
		char *name = NULL, *value = NULL;

		if( cszrawsentences >= CVOXFILESENTENCEMAX )
			break;

		for( ; p < last && ( *p == '\n' || *p == '\r' || *p == '\t' || *p == ' ' );
		     p++ );

		if( *p != '/' )
		{
			name = p;

			for( ; p < last && *p != ' ' && *p != '\t' ; p++ );

			if( p < last )
				*p++ = 0;

			value = p;
		}

		for( ; p < last && *p != '\n' && *p != '\r'; p++ );

		if( p < last )
			*p++ = 0;

		if( name )
		{
			int index = cszrawsentences;
			int size = strlen( name ) + strlen( value ) + 2;

			rgpszrawsentence[index] = Mem_Malloc( sndpool, size );
			memcpy( rgpszrawsentence[index], name, size );
			rgpszrawsentence[index][size - 1] = 0;
			cszrawsentences++;
		}
	}
}

static void VOX_ReadSentenceFile( const char *path )
{
	byte *buf;
	fs_offset_t size;

	VOX_Shutdown();

	buf = FS_LoadFile( path, &size, false );
	if( !buf ) return;

	VOX_ReadSentenceFile_( buf, size );

	Mem_Free( buf );
}

#if XASH_PS3
// Preload every VOX word referenced anywhere in sentences.txt, once per
// session -- not per map. Without this, each map's reslist.txt/.res re-announces the
// same session-lifetime word set through the full server precache -> network
// resource-list -> client registration walk (SV_CreateGenericResources,
// engine/server/sv_init.c, called on every single map load), even though
// S_FindName (s_load.c:191) already re-stamps servercount on every hit and
// S_FreeSounds (the only thing that would actually evict a cached sfx) is
// never called on a map transition -- so cached words already survive for
// free, but the hundreds-of-entries-per-map resource-list walk that
// re-registers them doesn't, and that walk is real cost on PS3's LV2
// filesystem. Loading the whole set once here removes the need for
// reslist.txt to carry VOX content at all -- that per-map cost disappears
// once every word is already resident before the first map even loads.
//
// Uses the exact same VOX_GetDirectory/VOX_ParseString word-expansion
// VOX_LoadSound (below) uses to build its S_FindName lookup key, so the
// cache key this populates is guaranteed to match what VOX_LoadWord will
// later search for -- deriving both from the same code path rules out the
// extension-mismatch class of bug the reslist.txt fix (engine/server/
// sv_init.c's SV_ReadResourceList) had to work around by construction,
// rather than by keeping two independent string-building paths in sync.
//
// Deferred to the first S_BeginRegistration (i.e. the first map load) rather
// than run from VOX_Init. The work is identical and the per-map saving above
// is unchanged from the second map onwards -- what moves is *when* it is paid.
// At VOX_Init time the engine is mid-boot with the menu not yet up, so the
// several seconds of small-file loads land in a window where nothing is drawn
// and the console looks hung. At first map load the same seconds land inside a
// window the player already expects to be a load, and SCR_BootProgress can
// draw over it. First map load gets slower by exactly what boot gets faster.
//
// This runs on EVERY map load, not only the first. S_EndRegistration
// (s_load.c) frees any sfx whose servercount != cl.servercount, and VOX words
// are deliberately absent from the per-map resource list -- that absence is
// the whole point of preloading them here. So the first map transition was
// freeing the entire preloaded set, and a once-only latch meant nothing ever
// brought it back: every spoken line after that reloaded from disk during
// gameplay, measured on hardware at 3-336ms per word, against a mixer cushion
// of only ~110ms. That is the audible stutter, and the underrun counters in
// engine/platform/ps3/s_ps3.c show it starting immediately after the first
// map change and never before it.
//
// Re-running costs almost nothing once the words are resident. This executes
// before s_registering goes true, so S_RegisterSound takes the S_LoadSound
// path, and S_LoadSound returns sfx->cache immediately for anything already
// loaded -- including names that failed to resolve, which cache the default
// sound rather than re-probing the filesystem. Its real work on a repeat pass
// is the servercount re-stamp in S_RegisterSound, which is exactly what makes
// the set survive the S_EndRegistration sweep later in the same map load.
static qboolean vox_preloaded = false;

// A word token can never legitimately contain whitespace: whitespace is
// precisely what VOX_ParseString splits on. So a token that still holds a
// space or a tab is proof the tokenizer failed on that sentence, not the name
// of an asset -- and probing for it costs a full four-format searchpath walk
// (wav/mp3/ogg/opus) that can only ever miss, measured at 128-270ms each on
// hardware against a mixer cushion of ~110ms.
//
// Tabs are the likely source: VOX_ParseString's two skip loops treat ' ', '.',
// ',' and '(' as separators but NOT '\t', so a tab-separated sentence body
// collapses into one token with the tabs still embedded. That matches the
// "vox/     " entry seen in hardware logs, where the terminal rendered the
// embedded whitespace indistinguishably from spaces.
//
// This is a preload-only guard and cannot change behaviour: VOX_ParseString
// and the gameplay VOX_LoadSound path are untouched, so if such a name somehow
// did resolve it would still be loaded lazily exactly as before. Fixing the
// tokenizer itself would alter how every sentence is split on every platform,
// which is not warranted for a PS3 preload cost -- and cannot be validated
// without the sentences.txt that produced these tokens.
static qboolean VOX_WordTokenIsSane( const char *word, qboolean report )
{
	if( !word || !word[0] )
		return false; // VOX_ParseString returns 1 empty word for an empty body

	for( const char *c = word; *c; c++ )
	{
		if( *c != ' ' && *c != '\t' )
			continue;

		if( report )
		{
			// Escape the token so tabs are distinguishable from spaces in the
			// log -- that ambiguity is what made this hard to identify.
			char esc[64];
			size_t o = 0;

			for( const char *s = word; *s && o < sizeof( esc ) - 3; s++ )
			{
				if( *s == '\t' ) { esc[o++] = '\\'; esc[o++] = 't'; }
				else if( *s == ' ' ) { esc[o++] = '\\'; esc[o++] = 's'; }
				else esc[o++] = *s;
			}
			esc[o] = 0;

			Con_Printf( S_WARN "%s: skipping malformed word token \"%s\" (whitespace in token)\n",
				__func__, esc );
		}

		return false;
	}

	return true;
}

void VOX_PreloadDeferred( void )
{
	int preloaded = 0;
	int skipped = 0;
	// Progress UI and the summary line are first-run only: on later maps this
	// loop is a cache walk that finishes far too fast to be worth drawing,
	// and redrawing 1065 progress steps would cost more than the work itself.
	qboolean first_run = !vox_preloaded;

	vox_preloaded = true;

	if( first_run )
		SCR_BootProgress( "Precaching speech", 0, cszrawsentences, true );

	for( int i = 0; i < cszrawsentences; i++ )
	{
		if( first_run )
			SCR_BootProgress( "Precaching speech", i, cszrawsentences, false );

		char buffer[512] = { 0 }, szpath[32] = { 0 };
		char *rgpparseword[CVOXWORDMAX] = { 0 };
		int len = Q_strlen( rgpszrawsentence[i] );
		const char *psz = &rgpszrawsentence[i][len + 1];

		for( ; *psz == ' ' || *psz == '\t'; psz++ );

		psz = VOX_GetDirectory( szpath, psz, sizeof( szpath ));
		if( !psz )
			continue;

		if( Q_strlen( psz ) >= sizeof( buffer ))
			continue;

		Q_strncpy( buffer, psz, sizeof( buffer ));
		int num_words = VOX_ParseString( buffer, rgpparseword );

		for( int w = 0; w < num_words; w++ )
		{
			voxword_t dummy;
			voxword_t default_voxword;
			VOX_MakeDefaultWordParams( &default_voxword );

			if( !VOX_ParseWordParams( rgpparseword[w], &dummy, &default_voxword ))
				continue;

			if( !VOX_WordTokenIsSane( rgpparseword[w], first_run ))
			{
				skipped++;
				continue;
			}

			char pathbuffer[MAX_SYSPATH];
			if( Q_snprintf( pathbuffer, sizeof( pathbuffer ), "%s%s", szpath, rgpparseword[w] ) < 0 )
				continue;

			if( S_RegisterSound( pathbuffer ) >= 0 )
				preloaded++;
		}
	}

	if( first_run )
		Con_Printf( "VOX_PreloadDeferred: %d words preloaded from %d sentences, %d malformed tokens skipped\n",
			preloaded, cszrawsentences, skipped );
	else
		Con_Printf( "VOX_PreloadDeferred: %d words re-stamped for servercount %d\n", preloaded, cl.servercount );
}
#endif

void VOX_Init( void )
{
	VOX_ReadSentenceFile( DEFAULT_SOUNDPATH "sentences.txt" );
}

void VOX_Shutdown( void )
{
	for( int i = 0; i < cszrawsentences; i++ )
		Mem_Free( rgpszrawsentence[i] );

	cszrawsentences = 0;
#if XASH_PS3
	// the sfx cache does not survive this, and a changegame re-reads a
	// different sentences.txt -- so the next session must preload again
	vox_preloaded = false;
#endif
}

#if XASH_ENGINE_TESTS
#include "tests.h"

static void Test_VOX_GetDirectory( void )
{
	const char *data[] =
	{
		"", "", "vox/",
		"bark bark", "bark bark", "vox/",
		"barney/meow", "meow", "barney/",
		"/fvox/_period", "_period", "fvox/",
	};

	for( int i = 0; i < sizeof( data ) / sizeof( data[0] ); i += 3 )
	{
		string szpath;
		const char *p = VOX_GetDirectory( szpath, data[i+0], sizeof( szpath ));

		TASSERT_STR( p, data[i+1] );
		TASSERT_STR( szpath, data[i+2] );
	}
}

static void Test_VOX_LookupString( void )
{
	const char *p, *data[] =
	{
		"0", "123",
		"3", "SPAAACE",
		"-2", NULL,
		"404", NULL,
		"not found", NULL,
		"exactmatch", "123",
		"caseinsensitive", "456",
		"SentenceWithTabs", "789",
		"SentenceWithSpaces", "SPAAACE",
	};

	VOX_Shutdown();

	rgpszrawsentence[cszrawsentences++] = (char*)"exactmatch\000123";
	rgpszrawsentence[cszrawsentences++] = (char*)"CaseInsensitive\000456";
	rgpszrawsentence[cszrawsentences++] = (char*)"SentenceWithTabs\0\t\t\t789";
	rgpszrawsentence[cszrawsentences++] = (char*)"SentenceWithSpaces\0  SPAAACE";
	rgpszrawsentence[cszrawsentences++] = (char*)"SentenceWithTabsAndSpaces\0\t \t\t MEOW";

	for( int i = 0; i < sizeof( data ) / sizeof( data[0] ); i += 2 )
	{
		p = VOX_LookupString( data[i] );

		TASSERT_STR( p, data[i+1] );
	}

	cszrawsentences = 0;
}

static void Test_VOX_ParseString( void )
{
	char *rgpparseword[CVOXWORDMAX];
	const char *data[] =
	{
		"(p100) my ass is, heavy!(p80 t20) clik.",
		"(p100)", "my", "ass", "is", "_comma", "heavy!(p80 t20)", "clik", NULL,
		"freeman...",
		"freeman", "_period", NULL,
		// tabs separate words exactly like spaces. Before this was true, the
		// " \t " below produced a token that was literally "\t", which loaded
		// as a second of silence mid-sentence (see VOX_ParseString).
		"doctor \t freeman",
		"doctor", "freeman", NULL,
		"doctor\tfreeman",
		"doctor", "freeman", NULL,
	};
	int i = 0;

	while( i < sizeof( data ) / sizeof( data[0] ))
	{
		char buffer[4096];
		int wordcount, j = 0;
		Q_strncpy( buffer, data[i], sizeof( buffer ));
		wordcount = VOX_ParseString( buffer, rgpparseword );

		i++;

		while( data[i] )
		{
			TASSERT_STR( data[i], rgpparseword[j] );
			i++;
			j++;
		}

		TASSERT( j == wordcount );

		i++;
	}
}

static void Test_VOX_ParseWordParams( void )
{
	string buffer;
	qboolean ret;
	voxword_t default_word;
	voxword_t word;

	VOX_MakeDefaultWordParams( &default_word );

	Q_strncpy( buffer, "heavy!(p80)", sizeof( buffer ));
	ret = VOX_ParseWordParams( buffer, &word, &default_word );
	TASSERT_STR( buffer, "heavy!" );
	TASSERT( word.pitch == 80 );
	TASSERT( ret );

	Q_strncpy( buffer, "(p105)", sizeof( buffer ));
	ret = VOX_ParseWordParams( buffer, &word, &default_word );
	TASSERT_STR( buffer, "" );
	TASSERT( word.pitch == 105 );
	TASSERT( !ret );

	Q_strncpy( buffer, "quiet(v50)", sizeof( buffer ));
	ret = VOX_ParseWordParams( buffer, &word, &default_word );
	TASSERT_STR( buffer, "quiet" );
	TASSERT( word.pitch == 105 ); // defaulted
	TASSERT( word.volume == 50 );
	TASSERT( ret );
}

void Test_RunVOX( void )
{
	TRUN( Test_VOX_GetDirectory() );
	TRUN( Test_VOX_LookupString() );
	TRUN( Test_VOX_ParseString() );
	TRUN( Test_VOX_ParseWordParams() );
}

#endif /* XASH_ENGINE_TESTS */
