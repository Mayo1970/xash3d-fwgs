/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
//
//  parsemsg.h
//
#pragma once
#define ASSERT( x )

#include <stdint.h>
#include <cstring>

class BufferReader
{
public:
	BufferReader( const char *name, void *buf, int size ) :
		m_szMsgName( name ), m_pBuf( (uint8_t*)buf ), m_iSize( size ), m_iRead( 0 ), m_bBad( false ) {}
	BufferReader( void *buf, int size ) : BufferReader( "not set", buf, size ) {}

#ifdef _DEBUG
	inline ~BufferReader( void );
#endif

	void Flush( void );
	bool Bad( void ) { return m_bBad; }
	bool Eof( void ) { return m_iRead >= m_iSize - 1; }

	bool Valid( void ) { return !Bad() && !Eof(); }

	template<typename T> T Read( void );

	int8_t ReadChar( void );
	uint8_t ReadByte( void );
	int16_t ReadShort( void );
	int16_t ReadWord( void );
	int32_t ReadLong( void ); // no mistake here, we assume that long is 32 bit.
	char *ReadString( void );
	float ReadFloat( void );
	float ReadCoord( void );
	float ReadAngle( void );
	float ReadHiResAngle( void );
	Vector ReadCoordVector( void );

private:
	const char *m_szMsgName;
	uint8_t *m_pBuf;
	size_t   m_iSize;
	size_t   m_iRead;
	bool     m_bBad;
	char     m_strBuf[2048];
};

inline void BufferReader::Flush( void )
{
	m_iRead = m_iSize - 1;
}

template<typename T>
inline T BufferReader::Read( void )
{
	if( m_bBad )
		return -1;

	// don't go out of bounds
	if( m_iRead + sizeof( T ) > m_iSize )
	{
		m_bBad = true;

		// may occur, but safe
		//gEngfuncs.Con_DPrintf( "BufferReader(%s): buffer overrun. Expected %i\n", m_szMsgName, m_iSize );
		return -1;
	}

	if( sizeof( T ) == 1 )
		return m_pBuf[m_iRead++];

	// The buffer holds LITTLE-ENDIAN wire data. Assembling it byte by byte is
	// the only host-neutral way to read it. The memcpy this replaced took the
	// raw bytes in host order, which is correct only on a little-endian target:
	// on big-endian (PS3/PPC64) it byte-reverses every short and long, so every
	// usermessage field wider than a byte came out garbage.
	// This is exactly what the stock HLSDK reader has always done -- see
	// READ_SHORT and READ_LONG in hlsdk-portable/cl_dll/parsemsg.cpp, which is
	// why the hlsdk-based flavors never hit this.
	// Only sizes 1, 2 and 4 are ever instantiated here (char*/float have their
	// own specializations below), so a 32-bit accumulator is wide enough.
	uint32_t raw = 0;

	for( size_t i = 0; i < sizeof( T ); i++ )
		raw |= (uint32_t)m_pBuf[m_iRead + i] << ( i * 8 );

	m_iRead += sizeof( T );

	return (T)raw;
}


template<>
inline char* BufferReader::Read( void )
{
	if( m_bBad )
		return (char*)""; // do not return NULL, may break strcpy's

	size_t l;
	for( l = 0; l < sizeof( m_strBuf ) - 1; l++)
	{
		if( m_iRead > m_iSize )
			break;

		int8_t c = ReadChar();
		if( c == -1 || c == 0 )
			break;

		m_strBuf[l] = c;
	}

	m_strBuf[l] = 0;

	return m_strBuf;
}

template<>
inline float BufferReader::Read( void )
{
	union
	{
		uint32_t u;
		float    f;
	} tr;

	if( m_bBad )
		return -1.0f;

	if( m_iRead + 4 > m_iSize )
	{
		m_bBad = true;
		return -1.0f;
	}

	// Same little-endian wire order as the integer path above. The byte-for-byte
	// copy this replaced put the wire's low byte into the sign/exponent position
	// on a big-endian host, so every float field read back as noise.
	tr.u = (uint32_t)m_pBuf[m_iRead]
		| ((uint32_t)m_pBuf[m_iRead + 1] << 8)
		| ((uint32_t)m_pBuf[m_iRead + 2] << 16)
		| ((uint32_t)m_pBuf[m_iRead + 3] << 24);

	m_iRead += 4;

	return tr.f;
}

inline int8_t BufferReader::ReadChar( void )
{
	return Read<int8_t>();
}

inline uint8_t BufferReader::ReadByte( void )
{
	return Read<uint8_t>();
}

inline int16_t BufferReader::ReadShort( void )
{
	return Read<int16_t>();
}

inline int16_t BufferReader::ReadWord( void )
{
	return ReadShort();
}

inline int32_t BufferReader::ReadLong( void )
{
	return Read<int32_t>();
}

inline char *BufferReader::ReadString( void )
{
	return Read<char*>();
}

inline float BufferReader::ReadFloat( void )
{
	return Read<float>();
}

inline float BufferReader::ReadCoord( void )
{
	return ReadShort() * 0.125f;
}

inline Vector BufferReader::ReadCoordVector( void )
{
	Vector v;
	v.x = ReadCoord();
	v.y = ReadCoord();
	v.z = ReadCoord();

	return v;
}

inline float BufferReader::ReadAngle( void )
{
	return ReadChar() * 360.0f / 256.0f;
}

inline float BufferReader::ReadHiResAngle( void )
{
	return ReadShort() * 360.0f / 65536.0f;
}

#ifdef _DEBUG
BufferReader::~BufferReader()
{
	//if( m_iRead < m_iSize - 1 )
	//	gEngfuncs.Con_DPrintf( "BufferReader(%s): destroyed before reaching end. Expected %i, read %i\n", m_szMsgName, m_iSize, m_iRead );
}
#endif
