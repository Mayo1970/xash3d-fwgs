//SHA.h 

#ifndef __SHA_H__ 
#define __SHA_H__ 

#include <stdbool.h>

//Typical DISCLAIMER: 
//The code in this project is Copyright (C) 2003 by George Anescu. You have the right to 
//use and distribute the code in any way you see fit as long as this paragraph is included 
//with the distribution. No warranties or claims are made as to the validity of the 
//information and code contained herein, so use it at your own risk.

enum { BLOCKSIZE = 64, BLOCKSIZE2 = BLOCKSIZE<<1 };
//For 32 bits Integers
enum { SHA256LENGTH=8 };

typedef struct CSHA
{
	bool m_bAddData;
	//Context Variables
	unsigned int m_auiBuf[SHA256LENGTH]; //Maximum for SHA256
	unsigned int m_auiBits[2];
	unsigned char m_aucIn[BLOCKSIZE2]; //128 bytes for SHA384, SHA512
} CSHA;

void CSHA_Reset( CSHA *ctx );
//Update context to reflect the concatenation of another buffer of bytes.
void CSHA_AddData( CSHA *ctx, char const* pcData, unsigned int iDataLength );
//Final wrapup - pad to 64-byte boundary with the bit pattern
//1 0*(64-bit count of bits processed, MSB-first)
void CSHA_FinalDigest( CSHA *ctx, char* pcDigest );

#endif // __SHA_H__ 
