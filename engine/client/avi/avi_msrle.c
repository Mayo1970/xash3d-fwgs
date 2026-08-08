/*
avi_msrle.c - Microsoft RLE (8-bit) video decoder for the native AVI backend
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

// MSRLE rows run bottom-up (positive biHeight), so row y of the bitstream is
// row height-1-y of the image
static uint32_t *MSRLE_Row( uint32_t *dst, int width, int height, int y )
{
	if( y < 0 || y >= height )
		return NULL;

	return dst + ( height - 1 - y ) * width;
}

/*
================
MSRLE_Decode

Decodes one frame in place into dst (BGRA32, tightly packed). Untouched
pixels keep the previous frame, as delta frames require.
================
*/
qboolean MSRLE_Decode( const byte *data, size_t size, const uint32_t *palette, uint32_t *dst, int width, int height )
{
	const byte *end = data + size;
	int x = 0, y = 0;

	while( end - data >= 2 )
	{
		int count = *data++;
		int value = *data++;

		if( count > 0 )
		{
			// encoded mode: a run of one palette index
			uint32_t *row = MSRLE_Row( dst, width, height, y );
			uint32_t pix = palette[value];
			int i;

			if( row )
			{
				for( i = 0; i < count && x + i < width; i++ )
					row[x + i] = pix;
			}

			x += count;
			continue;
		}

		switch( value )
		{
		case 0: // end of line
			x = 0;
			y++;
			break;
		case 1: // end of bitmap
			return true;
		case 2: // delta: skip dx, dy
			if( end - data < 2 )
				return true;
			x += *data++;
			y += *data++;
			break;
		default: // absolute mode: `value` literal indices, padded to a word
			{
				uint32_t *row = MSRLE_Row( dst, width, height, y );
				int i;

				if( end - data < value )
					return true;

				if( row )
				{
					for( i = 0; i < value && x + i < width; i++ )
						row[x + i] = palette[data[i]];
				}

				data += value;
				x += value;

				if( value & 1 )
					data++; // pad byte
			}
			break;
		}
	}

	return true;
}

#endif // XASH_AVI == AVI_NATIVE
