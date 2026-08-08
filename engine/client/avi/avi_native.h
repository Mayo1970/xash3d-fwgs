/*
avi_native.h - native AVI backend, shared decoder interface
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
#ifndef AVI_NATIVE_H
#define AVI_NATIVE_H

#if XASH_AVI == AVI_NATIVE

// decoders write straight into the buffer the engine uploads as PF_BGRA_32,
// i.e. bytes B, G, R, A in memory order. Build the word so that the store
// lands in that order on either endianness
#if XASH_BIG_ENDIAN
#define AVI_PACK_BGRA( b, g, r ) ((((uint32_t)( b )) << 24 ) | (((uint32_t)( g )) << 16 ) | (((uint32_t)( r )) << 8 ) | 0xFFU )
#else
#define AVI_PACK_BGRA( b, g, r ) ((0xFFU << 24 ) | (((uint32_t)( r )) << 16 ) | (((uint32_t)( g )) << 8 ) | ((uint32_t)( b )))
#endif

//
// avi_cinepak.c
//
typedef struct cinepak_s cinepak_t;

cinepak_t *Cinepak_Create( poolhandle_t pool );
void Cinepak_Destroy( cinepak_t *cin );
qboolean Cinepak_Decode( cinepak_t *cin, const byte *data, size_t size, uint32_t *dst, int width, int height );

//
// avi_msrle.c
//
qboolean MSRLE_Decode( const byte *data, size_t size, const uint32_t *palette, uint32_t *dst, int width, int height );

#endif // XASH_AVI == AVI_NATIVE
#endif // AVI_NATIVE_H
