/*
avi_native.c - playing AVI files (native backend, no ffmpeg)
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

#include "defaults.h"
#include "common.h"
#include "client.h"
#include "avi_native.h"

#if XASH_AVI == AVI_NATIVE

static qboolean avi_initialized;
static poolhandle_t avi_mempool;

// RIFF is little-endian, so every id is read low byte first
#define AVI_FOURCC( a, b, c, d ) ((uint32_t)(byte)( a ) | ((uint32_t)(byte)( b ) << 8 ) | ((uint32_t)(byte)( c ) << 16 ) | ((uint32_t)(byte)( d ) << 24 ))

#define ID_RIFF AVI_FOURCC( 'R', 'I', 'F', 'F' )
#define ID_AVI  AVI_FOURCC( 'A', 'V', 'I', ' ' )
#define ID_LIST AVI_FOURCC( 'L', 'I', 'S', 'T' )
#define ID_HDRL AVI_FOURCC( 'h', 'd', 'r', 'l' )
#define ID_MOVI AVI_FOURCC( 'm', 'o', 'v', 'i' )
#define ID_STRL AVI_FOURCC( 's', 't', 'r', 'l' )
#define ID_AVIH AVI_FOURCC( 'a', 'v', 'i', 'h' )
#define ID_STRH AVI_FOURCC( 's', 't', 'r', 'h' )
#define ID_STRF AVI_FOURCC( 's', 't', 'r', 'f' )
#define ID_VIDS AVI_FOURCC( 'v', 'i', 'd', 's' )
#define ID_AUDS AVI_FOURCC( 'a', 'u', 'd', 's' )
#define ID_CVID AVI_FOURCC( 'c', 'v', 'i', 'd' )

// biCompression values that aren't fourccs
#define BI_RGB  0
#define BI_RLE8 1

#define WAVE_FORMAT_PCM 1

// hard ceiling on buffered-but-unplayed audio, in case nothing is draining it
#define AVI_MAX_CACHED_AUDIO ( 2 * 1024 * 1024 )

struct movie_state_s
{
	// whole file in main RAM. Both intro clips are single-digit megabytes and
	// per-frame LV2 reads are the platform's documented latency trap
	byte  *file;
	size_t file_size;

	// demuxer cursor inside LIST movi
	size_t movi_start;
	size_t movi_end;
	size_t pos;

	// video stream
	int      video_stream;
	uint32_t codec;   // biCompression
	uint32_t handler; // strh fccHandler, used when biCompression is BI_RGB
	int      bitcount;
	int      xres, yres;
	int      frame_count;
	int      cur_frame;
	double   frame_time;
	double   duration;
	uint32_t *frame;       // BGRA32, persists between frames (inter coding)
	uint32_t palette[256];
	cinepak_t *cinepak;

	// audio stream
	int audio_stream;
	int rate;
	int channels;
	int width; // bytes per sample

	byte  *cached_audio;
	size_t cached_audio_buf_len;
	size_t cached_audio_len;
	size_t cached_audio_pos;

	double first_time;

	// rendering video parameters
	int x, y, w, h;
	int texture;

	// rendering audio parameters
	float   attn;
	int16_t entnum;
	byte    volume;
	byte    active : 1;
	byte    quiet  : 1;
	byte    paused : 1;
	byte    eof    : 1;
};

static uint32_t rd_le16( const byte *p )
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8 );
}

static uint32_t rd_le32( const byte *p )
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8 ) | ((uint32_t)p[2] << 16 ) | ((uint32_t)p[3] << 24 );
}

static void AVI_SpewError( qboolean quiet, const char *fmt, ... ) FORMAT_CHECK( 2 );
static void AVI_SpewError( qboolean quiet, const char *fmt, ... )
{
	char buf[MAX_VA_STRING];
	va_list va;

	if( quiet )
		return;

	va_start( va, fmt );
	Q_vsnprintf( buf, sizeof( buf ), fmt, va );
	va_end( va );

	Con_Printf( S_ERROR "%s", buf );
}

/*
=================================================================

RIFF PARSING

=================================================================
*/
static void AVI_ParseStrl( movie_state_t *Avi, int stream, const byte *p, size_t size )
{
	const byte *end = p + size;
	uint32_t type = 0, scale = 1, rate = 0, length = 0;

	while( end - p >= 8 )
	{
		uint32_t id = rd_le32( p );
		uint32_t sz = rd_le32( p + 4 );
		const byte *body = p + 8;

		if( sz > (uint32_t)( end - body ))
			sz = end - body;

		if( id == ID_STRH && sz >= 36 )
		{
			type   = rd_le32( body );
			scale  = rd_le32( body + 20 );
			rate   = rd_le32( body + 24 );
			length = rd_le32( body + 32 );

			if( type == ID_VIDS )
				Avi->handler = rd_le32( body + 4 );
		}
		else if( id == ID_STRF && type == ID_VIDS && Avi->video_stream < 0 && sz >= 40 )
		{
			uint32_t bi_size = rd_le32( body );
			int      height  = (int)rd_le32( body + 8 );

			Avi->video_stream = stream;
			Avi->xres     = (int)rd_le32( body + 4 );
			Avi->yres     = height < 0 ? -height : height;
			Avi->bitcount = (int)rd_le16( body + 14 );
			Avi->codec    = rd_le32( body + 16 );

			if( scale != 0 && rate != 0 )
				Avi->frame_time = (double)scale / (double)rate;

			if( length != 0 )
				Avi->frame_count = (int)length;

			// paletted video keeps its palette right behind the header
			if( Avi->bitcount <= 8 && bi_size >= 40 && sz > bi_size )
			{
				uint32_t count = ( sz - bi_size ) / 4;
				uint32_t i;

				if( count > 256 )
					count = 256;

				for( i = 0; i < count; i++ )
				{
					const byte *e = body + bi_size + i * 4; // RGBQUAD: B, G, R, reserved
					Avi->palette[i] = AVI_PACK_BGRA( e[0], e[1], e[2] );
				}
			}
		}
		else if( id == ID_STRF && type == ID_AUDS && Avi->audio_stream < 0 && sz >= 16 )
		{
			int format   = (int)rd_le16( body );
			int channels = (int)rd_le16( body + 2 );
			int bits     = (int)rd_le16( body + 14 );

			// S_RawSamplesStereo consumes 8-bit unsigned or 16-bit signed PCM
			// directly, mono or stereo, so plain PCM needs no conversion at all
			if( format == WAVE_FORMAT_PCM && ( bits == 8 || bits == 16 ) && ( channels == 1 || channels == 2 ))
			{
				Avi->audio_stream = stream;
				Avi->channels     = channels;
				Avi->rate         = (int)rd_le32( body + 4 );
				Avi->width        = bits / 8;
			}
			else
			{
				AVI_SpewError( Avi->quiet, "AVI: unsupported audio format %i (%i bits, %i channels), playing silent\n",
					format, bits, channels );
			}
		}

		p = body + sz + ( sz & 1 );
	}
}

static void AVI_ParseHdrl( movie_state_t *Avi, const byte *p, size_t size )
{
	const byte *end = p + size;
	int stream = 0;

	while( end - p >= 8 )
	{
		uint32_t id = rd_le32( p );
		uint32_t sz = rd_le32( p + 4 );
		const byte *body = p + 8;

		if( sz > (uint32_t)( end - body ))
			sz = end - body;

		if( id == ID_AVIH && sz >= 32 )
		{
			uint32_t usec = rd_le32( body );

			if( usec != 0 )
				Avi->frame_time = (double)usec / 1000000.0;

			Avi->frame_count = (int)rd_le32( body + 16 );
		}
		else if( id == ID_LIST && sz >= 4 && rd_le32( body ) == ID_STRL )
		{
			AVI_ParseStrl( Avi, stream, body + 4, sz - 4 );
			stream++;
		}

		p = body + sz + ( sz & 1 );
	}
}

static qboolean AVI_ParseHeaders( movie_state_t *Avi )
{
	const byte *p = Avi->file;
	const byte *end = Avi->file + Avi->file_size;
	uint32_t riff_size;

	if( Avi->file_size < 12 || rd_le32( p ) != ID_RIFF || rd_le32( p + 8 ) != ID_AVI )
		return false;

	riff_size = rd_le32( p + 4 );

	if( riff_size + 8 < Avi->file_size )
		end = p + riff_size + 8;

	p += 12;

	while( end - p >= 8 )
	{
		uint32_t id = rd_le32( p );
		uint32_t sz = rd_le32( p + 4 );
		const byte *body = p + 8;

		if( sz > (uint32_t)( end - body ))
			sz = end - body;

		if( id == ID_LIST && sz >= 4 )
		{
			uint32_t type = rd_le32( body );

			if( type == ID_HDRL )
			{
				AVI_ParseHdrl( Avi, body + 4, sz - 4 );
			}
			else if( type == ID_MOVI )
			{
				Avi->movi_start = body + 4 - Avi->file;
				Avi->movi_end   = body + sz - Avi->file;
			}
		}

		p = body + sz + ( sz & 1 );
	}

	return Avi->movi_end > Avi->movi_start;
}

/*
=================================================================

DEMUXING & DECODING

=================================================================
*/
static int AVI_ChunkStream( const byte *p )
{
	if( p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' )
		return -1;

	return ( p[0] - '0' ) * 10 + ( p[1] - '0' );
}

static void AVI_DecodeVideo( movie_state_t *Avi, const byte *data, size_t size )
{
	if( size == 0 )
		return; // dropped frame, previous one stays on screen

	if( Avi->cinepak )
		Cinepak_Decode( Avi->cinepak, data, size, Avi->frame, Avi->xres, Avi->yres );
	else
		MSRLE_Decode( data, size, Avi->palette, Avi->frame, Avi->xres, Avi->yres );
}

static void AVI_CacheAudio( movie_state_t *Avi, const byte *data, size_t len )
{
	if( len == 0 )
		return;

	if( Avi->cached_audio_len - Avi->cached_audio_pos > AVI_MAX_CACHED_AUDIO )
	{
		// nothing is draining us (sound off, or the movie outran the mixer)
		Avi->cached_audio_len = Avi->cached_audio_pos = 0;
	}

	if( !Avi->cached_audio )
	{
		Avi->cached_audio_buf_len = Q_max( len, 16384 );
		Avi->cached_audio = Mem_Malloc( avi_mempool, Avi->cached_audio_buf_len );
		Avi->cached_audio_len = Avi->cached_audio_pos = 0;
	}
	else
	{
		if( Avi->cached_audio_pos != 0 )
		{
			Avi->cached_audio_len -= Avi->cached_audio_pos;
			memmove( Avi->cached_audio, Avi->cached_audio + Avi->cached_audio_pos, Avi->cached_audio_len );
			Avi->cached_audio_pos = 0;
		}

		if( len + Avi->cached_audio_len > Avi->cached_audio_buf_len )
		{
			Avi->cached_audio_buf_len = len + Avi->cached_audio_len;
			Avi->cached_audio = Mem_Realloc( avi_mempool, Avi->cached_audio, Avi->cached_audio_buf_len );
		}
	}

	memcpy( Avi->cached_audio + Avi->cached_audio_len, data, len );
	Avi->cached_audio_len += len;
}

/*
================
AVI_DemuxChunk

Consumes chunks until one belongs to a stream we care about. Returns false
at the end of the movi list.
================
*/
static qboolean AVI_DemuxChunk( movie_state_t *Avi, qboolean *got_video )
{
	while( Avi->pos + 8 <= Avi->movi_end )
	{
		const byte *p = Avi->file + Avi->pos;
		uint32_t sz = rd_le32( p + 4 );
		const byte *body = p + 8;
		int stream;

		if( rd_le32( p ) == ID_LIST )
		{
			// 'rec ' groups: step into it and keep going
			Avi->pos += 12;
			continue;
		}

		if( sz > (uint32_t)( Avi->movi_end - Avi->pos - 8 ))
			sz = Avi->movi_end - Avi->pos - 8;

		Avi->pos += 8 + sz + ( sz & 1 );

		stream = AVI_ChunkStream( p );

		if( stream < 0 )
			continue; // JUNK and friends

		if( stream == Avi->video_stream && p[2] == 'd' && ( p[3] == 'c' || p[3] == 'b' ))
		{
			AVI_DecodeVideo( Avi, body, sz );
			*got_video = true;
			return true;
		}

		if( stream == Avi->audio_stream && p[2] == 'w' && p[3] == 'b' )
		{
			AVI_CacheAudio( Avi, body, sz );
			return true;
		}
	}

	return false;
}

/*
=================================================================

PLAYBACK

=================================================================
*/
static void AVI_StreamAudio( movie_state_t *Avi )
{
	rawchan_t *ch;

	// keep the same semantics, when S_RAW_SOUND_SOUNDTRACK doesn't play if S_StartStreaming wasn't enabled
	qboolean disable_stream = Avi->entnum == S_RAW_SOUND_SOUNDTRACK ? !snd.streaming : false;

	if( !snd.initialized || disable_stream || cl.paused || !Avi->cached_audio )
		return;

	ch = S_FindRawChannel( Avi->entnum, true );

	if( !ch )
		return;

	ch->master_vol = Avi->volume;
	ch->dist_mult = ( Avi->attn / SND_CLIP_DISTANCE );

	if( ch->s_rawend < snd.soundtime )
		ch->s_rawend = snd.soundtime;

	while( ch->s_rawend < snd.soundtime + ch->max_samples )
	{
		int buffer_samples = ch->max_samples - ( ch->s_rawend - snd.soundtime );
		int file_samples = buffer_samples * ((float)Avi->rate / SOUND_OUTPUT_SPEED );
		int file_bytes;
		size_t copy;

		if( file_samples <= 1 ) return; // no more samples need

		file_bytes = file_samples * Avi->width * Avi->channels;

		if( file_bytes > ch->max_samples )
		{
			file_bytes = ch->max_samples;
			file_samples = file_bytes / ( Avi->width * Avi->channels );
		}

		copy = Q_min( file_bytes, Q_max( Avi->cached_audio_len - Avi->cached_audio_pos, 0 ));

		if( !copy )
			break;

		if( file_bytes > copy )
		{
			file_bytes = copy;
			file_samples = file_bytes / ( Avi->width * Avi->channels );
		}

		ch->s_rawend = S_RawSamplesStereo( ch->rawsamples, ch->s_rawend, ch->max_samples, file_samples,
			Avi->rate, Avi->width, Avi->channels, Avi->cached_audio + Avi->cached_audio_pos );
		Avi->cached_audio_pos += copy;
	}
}

static void AVI_DrawFrame( movie_state_t *Avi, qboolean redraw )
{
	if( Avi->texture == 0 )
	{
		int cinTexture = SCR_GetCinematicTexture();
		int w = Avi->w >= 0 ? Avi->w : refState.width;
		int h = Avi->h >= 0 ? Avi->h : refState.height;

		if( redraw )
			ref.dllFuncs.GL_UpdateTexture( cinTexture, Avi->xres, Avi->yres, Avi->xres, Avi->yres, (const byte *)Avi->frame, PF_BGRA_32 );

		ref.dllFuncs.R_DrawStretchPic( Avi->x, Avi->y, w, h, 0, 0, 1, 1, cinTexture );
	}
	else if( redraw && Avi->texture > 0 )
	{
		ref.dllFuncs.GL_UpdateTexture( Avi->texture, Avi->xres, Avi->yres, Avi->w, Avi->h, (const byte *)Avi->frame, PF_BGRA_32 );
	}
}

qboolean AVI_Think( movie_state_t *Avi )
{
	qboolean redraw = false;
	double now;
	int target;

	if( !Avi || !Avi->active )
		return false;

	// never store engine time in a float on this platform, it is epoch-scale
	now = Platform_DoubleTime();

	if( Avi->first_time == 0.0 )
		Avi->first_time = now;

	if( Avi->paused )
	{
		// hold the playhead where it is
		Avi->first_time = now - (double)Avi->cur_frame * Avi->frame_time;
		AVI_DrawFrame( Avi, false );
		return true;
	}

	AVI_StreamAudio( Avi );

	target = (int)(( now - Avi->first_time ) / Avi->frame_time );

	while( !Avi->eof && Avi->cur_frame <= target )
	{
		qboolean got_video = false;

		if( !AVI_DemuxChunk( Avi, &got_video ))
		{
			Avi->eof = true;
			break;
		}

		if( got_video )
		{
			Avi->cur_frame++;
			redraw = true;
		}

		AVI_StreamAudio( Avi );
	}

	AVI_DrawFrame( Avi, redraw );

	if( Avi->eof )
		return false;

	return true;
}

qboolean AVI_SetParm( movie_state_t *Avi, enum movie_parms_e parm, ... )
{
	qboolean ret = true;
	va_list va;

	if( !Avi )
		return false;

	va_start( va, parm );

	while( parm != AVI_PARM_LAST )
	{
		float fval;
		int val;

		switch( parm )
		{
		case AVI_RENDER_TEXNUM:
			Avi->texture = va_arg( va, int );
			break;
		case AVI_RENDER_X:
			Avi->x = va_arg( va, int );
			break;
		case AVI_RENDER_Y:
			Avi->y = va_arg( va, int );
			break;
		case AVI_RENDER_W:
			Avi->w = va_arg( va, int );
			break;
		case AVI_RENDER_H:
			Avi->h = va_arg( va, int );
			break;
		case AVI_REWIND:
			Avi->pos = Avi->movi_start;
			Avi->cur_frame = 0;
			Avi->first_time = 0.0;
			Avi->eof = false;
			Avi->cached_audio_len = Avi->cached_audio_pos = 0;
			break;
		case AVI_ENTNUM:
			val = va_arg( va, int );
			Avi->entnum = bound( 0, val, MAX_EDICTS );
			break;
		case AVI_VOLUME:
			val = va_arg( va, int );
			Avi->volume = bound( 0, val, 255 );
			break;
		case AVI_ATTN:
			fval = va_arg( va, double );
			Avi->attn = Q_max( 0.0f, fval );
			break;
		case AVI_PAUSE:
			Avi->paused = true;
			break;
		case AVI_RESUME:
			Avi->paused = false;
			break;
		default:
			ret = false;
		}

		parm = va_arg( va, enum movie_parms_e );
	}

	va_end( va );

	return ret;
}

int AVI_GetVideoFrameNumber( movie_state_t *Avi, float time )
{
	return 0;
}

byte *AVI_GetVideoFrame( movie_state_t *Avi, int frame )
{
	return (byte *)Avi->frame;
}

qboolean AVI_GetVideoInfo( movie_state_t *Avi, int *xres, int *yres, float *duration )
{
	if( !Avi || !Avi->active )
		return false;

	if( xres )
		*xres = Avi->xres;

	if( yres )
		*yres = Avi->yres;

	if( duration )
		*duration = Avi->duration;

	return true;
}

qboolean AVI_HaveAudioTrack( const movie_state_t *Avi )
{
	return Avi ? Avi->active && Avi->audio_stream >= 0 : false;
}

void AVI_OpenVideo( movie_state_t *Avi, const char *filename, qboolean load_audio, int quiet )
{
	fs_offset_t len = 0;
	int i, total;

	if( Avi->active )
		AVI_CloseVideo( Avi );

	if( !filename || !avi_initialized )
		return;

	memset( Avi, 0, sizeof( *Avi ));
	Avi->quiet = quiet;
	Avi->video_stream = Avi->audio_stream = -1;
	Avi->frame_time = 1.0 / 15.0; // until the headers say otherwise

	// the caller hands us a disk path (FS_GetDiskPath), which is relative to
	// the working directory -- go through the engine's own file layer, stdio
	// can't open those on PS3
	Avi->file = FS_LoadDirectFile( filename, &len );

	if( !Avi->file || len <= 0 )
	{
		AVI_SpewError( quiet, "AVI: couldn't open %s\n", filename );
		AVI_CloseVideo( Avi );
		return;
	}

	Avi->file_size = (size_t)len;

	if( !AVI_ParseHeaders( Avi ) || Avi->video_stream < 0 || Avi->xres <= 0 || Avi->yres <= 0 )
	{
		AVI_SpewError( quiet, "AVI: %s is not a supported AVI file\n", filename );
		AVI_CloseVideo( Avi );
		return;
	}

	if( Avi->codec == ID_CVID || Avi->handler == ID_CVID )
	{
		Avi->cinepak = Cinepak_Create( avi_mempool );
	}
	else if( Avi->codec == BI_RLE8 && Avi->bitcount == 8 )
	{
		; // MSRLE, palette already read
	}
	else
	{
		AVI_SpewError( quiet, "AVI: %s uses an unsupported codec (%c%c%c%c, %i bpp)\n", filename,
			(char)( Avi->codec & 0xFF ), (char)(( Avi->codec >> 8 ) & 0xFF ),
			(char)(( Avi->codec >> 16 ) & 0xFF ), (char)(( Avi->codec >> 24 ) & 0xFF ), Avi->bitcount );
		AVI_CloseVideo( Avi );
		return;
	}

	if( !load_audio )
		Avi->audio_stream = -1;

	total = Avi->xres * Avi->yres;
	Avi->frame = Mem_Malloc( avi_mempool, total * sizeof( uint32_t ));

	for( i = 0; i < total; i++ )
		Avi->frame[i] = AVI_PACK_BGRA( 0, 0, 0 );

	Avi->pos      = Avi->movi_start;
	Avi->duration = Avi->frame_count * Avi->frame_time;
	Avi->entnum   = S_RAW_SOUND_SOUNDTRACK;
	Avi->attn     = ATTN_NONE;
	Avi->volume   = 255;
	Avi->active   = true;

	Con_Reportf( "AVI: %s, %ix%i, %.2f fps, %i frames%s\n", filename, Avi->xres, Avi->yres,
		1.0 / Avi->frame_time, Avi->frame_count, Avi->audio_stream >= 0 ? ", with audio" : "" );
}

void AVI_CloseVideo( movie_state_t *Avi )
{
	if( !Avi )
		return;

	if( Avi->cinepak )
		Cinepak_Destroy( Avi->cinepak );

	if( Avi->frame )
		Mem_Free( Avi->frame );

	if( Avi->cached_audio )
		Mem_Free( Avi->cached_audio );

	if( Avi->file )
		Mem_Free( Avi->file );

	memset( Avi, 0, sizeof( *Avi ));
}

/*
=================================================================

SHARED STATE

mirrors the tail of avi_ffmpeg.c -- it can't be shared between backends
because it needs the complete movie_state_s

=================================================================
*/
static movie_state_t avi[2];

movie_state_t *AVI_GetState( int num )
{
	return &avi[num];
}

qboolean AVI_IsActive( movie_state_t *Avi )
{
	return Avi ? Avi->active : false;
}

qboolean AVI_Initailize( void )
{
	if( Sys_CheckParm( "-noavi" ))
	{
		Con_Printf( "AVI: Disabled\n" );
		return false;
	}

	avi_initialized = true;
	avi_mempool = Mem_AllocPool( "AVI Zone" );

	return true;
}

void AVI_Shutdown( void )
{
	Mem_FreePool( &avi_mempool );
	avi_initialized = false;
}

movie_state_t *AVI_LoadVideo( const char *filename, qboolean load_audio )
{
	movie_state_t *Avi;
	string      path;
	const char *fullpath;

	// fast reject
	if( !avi_initialized )
		return NULL;

	// open cinematic
	Q_snprintf( path, sizeof( path ), "media/%s", filename );
	COM_DefaultExtension( path, ".avi", sizeof( path ));
	fullpath = FS_GetDiskPath( path, false );

	if( FS_FileExists( path, false ) && !fullpath )
	{
		Con_Printf( "Couldn't load %s from packfile. Please extract it\n", path );
		return NULL;
	}

	Avi = Mem_Calloc( avi_mempool, sizeof( movie_state_t ));
	AVI_OpenVideo( Avi, fullpath, load_audio, false );

	if( !AVI_IsActive( Avi ))
	{
		AVI_FreeVideo( Avi ); // something bad happens
		return NULL;
	}

	// all done
	return Avi;
}

void AVI_FreeVideo( movie_state_t *Avi )
{
	if( !Avi )
		return;

	AVI_CloseVideo( Avi );

	if( Mem_IsAllocatedExt( avi_mempool, Avi ))
		Mem_Free( Avi );
}

#endif // XASH_AVI == AVI_NATIVE
