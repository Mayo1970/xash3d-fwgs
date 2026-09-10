/*
 * BSD 3-Clause License
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include<VGUI_DataInputStream.h>
#include "minbase_endian.h"

using namespace vgui;

// TFC-5 hardware-crash fix: every multi-byte read below used to be a raw
// memcpy from the stream into a native-order variable, with no byteswap at
// all. VGUI1's own file formats (TGA headers via BitmapTGA, .res/BuildGroup
// files) are little-endian, same as every other GoldSrc-era asset format --
// on this big-endian PPU target that silently produced garbage values, most
// visibly BitmapTGA::loadTGA's wide/tall (read via readUShort), which then
// drove an unchecked `new uchar[wide*tall*4]` in Bitmap.cpp -- the actual
// crash the first HUD_VidInit-on-connect run hit. LittleWord/LittleInt16/
// LittleDWord/LittleInt32/LittleFloat (minbase_endian.h, already vendored
// alongside this library) are no-ops on little-endian hosts and swap only
// on big-endian ones, so this matches upstream behavior everywhere else.

DataInputStream::DataInputStream(InputStream* is)
{
	_is=is;
}

void DataInputStream::seekStart(bool& success)
{
	if(_is==null)
	{
		success=false;
		return;
	}

	_is->seekStart(success);
}

void DataInputStream::seekRelative(int count,bool& success)
{
	if(_is==null)
	{
		success=false;
		return;
	}

	_is->seekRelative(count,success);
}

void DataInputStream::seekEnd(bool& success)
{
	if(_is==null)
	{
		success=false;
		return;
	}

	_is->seekEnd(success);
}

int DataInputStream::getAvailable(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	return _is->getAvailable(success);
}

void DataInputStream::readUChar(uchar* buf,int count,bool& success)
{
	if(_is==null)
	{
		success=false;
		return;
	}

	_is->readUChar(buf,count,success);
}

void DataInputStream::close(bool& success)
{
	if(_is==null)
	{
		success=false;
		return;
	}

	_is->close(success);
}

void DataInputStream::close()
{
	bool success;
	_is->close(success);
}

bool DataInputStream::readBool(bool& success)
{
	if(_is==null)
	{
		success=false;
		return false;
	}

	return _is->readUChar(success)!=0;
}

char DataInputStream::readChar(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	return _is->readUChar(success);
}

uchar DataInputStream::readUChar(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	return _is->readUChar(success);
}

short DataInputStream::readShort(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	short ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleInt16(ret);
}

ushort DataInputStream::readUShort(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	ushort ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleWord(ret);
}

int DataInputStream::readInt(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	int ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleInt32(ret);
}

uint DataInputStream::readUInt(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	uint ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleDWord(ret);
}

long DataInputStream::readLong(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	// Read exactly 4 bytes, not sizeof(long) -- this format's "long" means
	// the classic 32-bit field every other GoldSrc-era format uses, but
	// `long` is 8 bytes on this LP64 (PPC64) target, which would silently
	// consume 4 bytes too many from the stream. Never actually called in
	// this tree today, but latent and worth getting right while touching
	// this file for the byteswap fix.
	int32 ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleInt32(ret);
}

ulong DataInputStream::readULong(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	// Same 4-byte-not-sizeof(long) fix as readLong above.
	uint32 ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleDWord(ret);
}

float DataInputStream::readFloat(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	float ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	return LittleFloat(ret);
}

double DataInputStream::readDouble(bool& success)
{
	if(_is==null)
	{
		success=false;
		return 0;
	}

	double ret;
	_is->readUChar((uchar*)&ret,sizeof(ret),success);
	// minbase_endian.h has no 8-byte-float macro (its LittleFloat is 4-byte
	// only) -- QWordSwap on the raw bit pattern is the same idiom.
#if __BYTE_ORDER == __BIG_ENDIAN
	ret = QWordSwap(ret);
#endif
	return ret;
}

void DataInputStream::readLine(char* buf,int bufLen,bool& success)
{
	if(_is==null)
	{
		success=false;
		return;
	}

	_is->readUChar((uchar*)buf,bufLen,success);
}
