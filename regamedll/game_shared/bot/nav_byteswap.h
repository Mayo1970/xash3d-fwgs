#pragma once

#include <string.h>

// The .nav binary format is always little-endian on disk (authored by Valve's
// PC tools and PC dedicated servers). This SDK carries its own private
// common/ and public/ trees (see regamedll/dlls/wscript's include list) that
// never reach the engine's LittleLong/LittleShort/LittleFloat -- and its own
// public/build.h, which does define XASH_BIG_ENDIAN under __PPU__, is never
// #included by anything in this tree either. So XASH_BIG_ENDIAN is set
// directly in regamedll/dlls/wscript for ps3 builds, same as XASH_64BIT
// above it, instead of relying on a header nothing here includes.

#ifdef XASH_BIG_ENDIAN

inline unsigned int NavSwap32( unsigned int v )
{
	return __builtin_bswap32( v );
}

inline unsigned short NavSwap16( unsigned short v )
{
	return __builtin_bswap16( v );
}

inline float NavSwapFloat( float v )
{
	unsigned int u;
	memcpy( &u, &v, sizeof( u ));
	u = NavSwap32( u );
	memcpy( &v, &u, sizeof( v ));
	return v;
}

#else

inline unsigned int NavSwap32( unsigned int v ) { return v; }
inline unsigned short NavSwap16( unsigned short v ) { return v; }
inline float NavSwapFloat( float v ) { return v; }

#endif // XASH_BIG_ENDIAN

// Swaps `count` contiguous floats in place (e.g. a 6-float Extent or a
// 3-float Vector read/written as one block).
inline void NavSwapFloats( float *v, int count )
{
	for ( int i = 0; i < count; i++ )
		v[i] = NavSwapFloat( v[i] );
}
