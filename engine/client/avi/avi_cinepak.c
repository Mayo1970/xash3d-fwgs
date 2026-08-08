/*
avi_cinepak.c - Cinepak (CVID) video decoder for the native AVI backend
Copyright (C) 2003 Dr. Tim Ferguson (for the original public decoder)
Copyright (C) 2003 The FFmpeg project (libavcodec/cinepak.c, LGPL-2.1-or-later)
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
#include "avi_native.h"

#if XASH_AVI == AVI_NATIVE

// a Cinepak frame is cut into horizontal strips, each carrying its own pair
// of codebooks. 32 is what every other decoder allows, and the whole thing
// is one pool allocation, so the ~256K sits in main RAM, not on the stack
#define CINEPAK_MAX_STRIPS 32

// chunk ids inside a strip: 0x0100 = selective update, 0x0200 = V1 book,
// 0x0400 = 4-byte (chroma-less) entries
#define CVID_CB_SELECTIVE  0x0100
#define CVID_CB_GRAYSCALE  0x0400

typedef struct
{
	// one packed BGRA texel per Y sample, converted once at codebook load
	// time so block writes are pure stores -- the PPE has no cycles to
	// spare for per-pixel YUV math at 640x480
	uint32_t pix[4];
} cvid_codebook_t;

typedef struct
{
	cvid_codebook_t v4[256];
	cvid_codebook_t v1[256];
	int y0, y1; // rows covered by this strip, [y0, y1)
} cvid_strip_t;

struct cinepak_s
{
	cvid_strip_t strips[CINEPAK_MAX_STRIPS];
};

// Cinepak is big-endian inside the little-endian RIFF container. Read it
// bytewise, never through a struct
static uint32_t rd_be16( const byte *p )
{
	return ((uint32_t)p[0] << 8 ) | p[1];
}

static uint32_t rd_be24( const byte *p )
{
	return ((uint32_t)p[0] << 16 ) | ((uint32_t)p[1] << 8 ) | p[2];
}

static uint32_t rd_be32( const byte *p )
{
	return ((uint32_t)p[0] << 24 ) | ((uint32_t)p[1] << 16 ) | ((uint32_t)p[2] << 8 ) | p[3];
}

static int cvid_clamp( int v )
{
	if( v < 0 ) return 0;
	if( v > 255 ) return 255;
	return v;
}

/*
================
Cinepak_Codebook

Cinepak chroma is stored pre-scaled (roughly 0.7 of BT.601 Cb/Cr), which is
why the reconstruction is the familiar r = y + 2v, b = y + 2u, g = y - u/2 - v
================
*/
static void Cinepak_Codebook( cvid_codebook_t *cb, int chunk_id, const byte *data, size_t size )
{
	const byte *end = data + size;
	const qboolean selective = FBitSet( chunk_id, CVID_CB_SELECTIVE ) ? true : false;
	const int entry_size = FBitSet( chunk_id, CVID_CB_GRAYSCALE ) ? 4 : 6;
	uint32_t flags = 0;
	int i, bit = 0;

	for( i = 0; i < 256; i++ )
	{
		int y[4], u = 0, v = 0, j;

		if( selective )
		{
			if( bit == 0 )
			{
				if( end - data < 4 )
					return;

				flags = rd_be32( data );
				data += 4;
				bit = 32;
			}

			bit--;

			if( !FBitSet( flags, 1U << bit ))
				continue; // entry unchanged
		}

		if( end - data < entry_size )
			return;

		y[0] = *data++;
		y[1] = *data++;
		y[2] = *data++;
		y[3] = *data++;

		if( entry_size == 6 )
		{
			u = (signed char)*data++;
			v = (signed char)*data++;
		}

		for( j = 0; j < 4; j++ )
		{
			int r = cvid_clamp( y[j] + v * 2 );
			int g = cvid_clamp( y[j] - u / 2 - v );
			int b = cvid_clamp( y[j] + u * 2 );

			cb[i].pix[j] = AVI_PACK_BGRA( b, g, r );
		}
	}
}

// one codebook entry covers the whole 4x4 macroblock, a Y sample per 2x2 quadrant
static void Cinepak_PutV1( uint32_t *dst, int w, int h, int x, int y, const cvid_codebook_t *cb )
{
	int i, j;

	for( j = 0; j < 4; j++ )
	{
		uint32_t *row;

		if( y + j >= h )
			break;

		row = dst + ( y + j ) * w + x;

		for( i = 0; i < 4; i++ )
		{
			if( x + i >= w )
				break;

			row[i] = cb->pix[( j & 2 ) + (( i & 2 ) >> 1 )];
		}
	}
}

// four codebook entries, one per 2x2 quadrant, each contributing all 4 samples
static void Cinepak_PutV4( uint32_t *dst, int w, int h, int x, int y, const cvid_codebook_t *v4, const byte *idx )
{
	int i, j;

	for( j = 0; j < 4; j++ )
	{
		uint32_t *row;

		if( y + j >= h )
			break;

		row = dst + ( y + j ) * w + x;

		for( i = 0; i < 4; i++ )
		{
			if( x + i >= w )
				break;

			row[i] = v4[idx[( j & 2 ) + (( i & 2 ) >> 1 )]].pix[(( j & 1 ) << 1 ) + ( i & 1 )];
		}
	}
}

static void Cinepak_Vectors( cvid_strip_t *strip, int chunk_id, const byte *data, size_t size, uint32_t *dst, int width, int height )
{
	const byte *end = data + size;
	uint32_t flags = 0;
	int bit = 0;
	int x, y;

	for( y = strip->y0; y < strip->y1; y += 4 )
	{
		for( x = 0; x < width; x += 4 )
		{
			qboolean v4;

			if( chunk_id == 0x3200 )
			{
				// V1-only vectors, no flag stream at all
				if( data >= end )
					return;

				Cinepak_PutV1( dst, width, height, x, y, &strip->v1[*data++] );
				continue;
			}

			if( bit == 0 )
			{
				if( end - data < 4 )
					return;

				flags = rd_be32( data );
				data += 4;
				bit = 32;
			}

			bit--;

			if( chunk_id == 0x3100 )
			{
				// inter-coded: first bit says whether the block is coded at
				// all. Uncoded blocks keep whatever the previous frame left
				// in dst, which is why the frame buffer must persist
				if( !FBitSet( flags, 1U << bit ))
					continue;

				if( bit == 0 )
				{
					if( end - data < 4 )
						return;

					flags = rd_be32( data );
					data += 4;
					bit = 32;
				}

				bit--;
			}

			v4 = FBitSet( flags, 1U << bit ) ? true : false;

			if( v4 )
			{
				if( end - data < 4 )
					return;

				Cinepak_PutV4( dst, width, height, x, y, strip->v4, data );
				data += 4;
			}
			else
			{
				if( data >= end )
					return;

				Cinepak_PutV1( dst, width, height, x, y, &strip->v1[*data++] );
			}
		}
	}
}

cinepak_t *Cinepak_Create( poolhandle_t pool )
{
	return Mem_Calloc( pool, sizeof( cinepak_t ));
}

void Cinepak_Destroy( cinepak_t *cin )
{
	if( cin )
		Mem_Free( cin );
}

/*
================
Cinepak_Decode

Decodes one frame in place into dst (BGRA32, tightly packed). dst keeps the
previous frame: inter-coded strips only touch the blocks they carry.
================
*/
qboolean Cinepak_Decode( cinepak_t *cin, const byte *data, size_t size, uint32_t *dst, int width, int height )
{
	const byte *end = data + size;
	uint32_t len;
	int strips, i, y0 = 0;
	int frame_flags;

	if( size < 10 )
		return false;

	frame_flags = data[0];
	len = rd_be24( data + 1 );
	strips = rd_be16( data + 8 );
	data += 10;

	if( len < size )
		end = data - 10 + len;

	if( strips > CINEPAK_MAX_STRIPS )
		strips = CINEPAK_MAX_STRIPS;

	for( i = 0; i < strips; i++ )
	{
		cvid_strip_t *strip = &cin->strips[i];
		const byte *strip_end;
		uint32_t strip_size, strip_height;

		if( end - data < 12 )
			break;

		// on a keyframe every strip past the first starts from its
		// predecessor's codebooks and only carries the differences -- without
		// this, selective updates (0x2100/0x2300) land on stale entries and
		// the frame comes out sprinkled with wrong blocks
		if( i > 0 && !FBitSet( frame_flags, 0x01 ))
		{
			memcpy( strip->v4, cin->strips[i - 1].v4, sizeof( strip->v4 ));
			memcpy( strip->v1, cin->strips[i - 1].v1, sizeof( strip->v1 ));
		}

		strip_size = rd_be16( data + 2 );

		// the header's y1 field is the strip height relative to the end of
		// the previous strip -- absolute coordinates are not written
		// consistently by encoders, so they're rebuilt by stacking
		strip_height = rd_be16( data + 8 );

		strip_end = data + strip_size;
		if( strip_size < 12 || strip_end > end )
			strip_end = end;

		strip->y0 = y0;
		strip->y1 = strip_height ? y0 + (int)strip_height : height;

		if( strip->y1 > height )
			strip->y1 = height;

		data += 12;

		while( strip_end - data >= 4 )
		{
			uint32_t chunk_id = rd_be16( data );
			uint32_t chunk_size = rd_be16( data + 2 );

			if( chunk_size < 4 )
				break;

			if( (size_t)( strip_end - data ) < chunk_size )
				chunk_size = strip_end - data;

			switch( chunk_id )
			{
			case 0x2000: case 0x2100: case 0x2400: case 0x2500:
				Cinepak_Codebook( strip->v4, chunk_id, data + 4, chunk_size - 4 );
				break;
			case 0x2200: case 0x2300: case 0x2600: case 0x2700:
				Cinepak_Codebook( strip->v1, chunk_id, data + 4, chunk_size - 4 );
				break;
			case 0x3000: case 0x3100: case 0x3200:
				Cinepak_Vectors( strip, chunk_id, data + 4, chunk_size - 4, dst, width, height );
				break;
			default:
				break; // unknown chunk, skip it
			}

			data += chunk_size;
		}

		data = strip_end;
		y0 = strip->y1;

		if( y0 >= height )
			break;
	}

	return true;
}

#endif // XASH_AVI == AVI_NATIVE
