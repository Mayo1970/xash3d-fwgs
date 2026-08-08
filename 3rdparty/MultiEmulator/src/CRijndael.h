#ifndef __RIJNDAEL_H__
#define __RIJNDAEL_H__

#include <string.h>
#include <stdbool.h>

//Rijndael (pronounced Reindaal) is a block cipher, designed by Joan Daemen and Vincent Rijmen as a candidate algorithm for the AES.
//The cipher has a variable block length and key length. The authors currently specify how to use keys with a length
//of 128, 192, or 256 bits to encrypt blocks with al length of 128, 192 or 256 bits (all nine combinations of
//key length and block length are possible). Both block length and key length can be extended very easily to
// multiples of 32 bits.
//Rijndael can be implemented very efficiently on a wide range of processors and in hardware. 
//This implementation is based on the Java Implementation used with the Cryptix toolkit found at:
//http://www.esat.kuleuven.ac.be/~rijmen/rijndael/rijndael.zip
//Java code authors: Raif S. Naffah, Paulo S. L. M. Barreto
//This Implementation was tested against KAT test published by the authors of the method and the
//results were identical.

enum { DEFAULT_BLOCK_SIZE = 16 };
enum { MAX_BLOCK_SIZE = 32, MAX_ROUNDS = 14, MAX_KC = 8, MAX_BC = 8 };

typedef struct CRijndael
{
	//Key Initialization Flag
	bool m_bKeyInit;
	//Encryption (m_Ke) round key
	int m_Ke[MAX_ROUNDS + 1][MAX_BC];
	//Decryption (m_Kd) round key
	int m_Kd[MAX_ROUNDS + 1][MAX_BC];
	//Key Length
	int m_keylength;
	//Block Size
	int	m_blockSize;
	//Number of Rounds
	int m_iROUNDS;
	//Chain Block
	char m_chain0[MAX_BLOCK_SIZE];
	char m_chain[MAX_BLOCK_SIZE];
	//Auxiliary private use buffers
	int tk[MAX_KC];
	int a[MAX_BC];
	int t[MAX_BC];
} CRijndael;

static void CRijndael_Init( CRijndael *ctx )
{
	ctx->m_bKeyInit = false;
}

//Expand a user-supplied key material into a session key.
// key        - The 128/192/256-bit user-key to use.
// chain      - initial chain block for CBC and CFB modes.
// keylength  - 16, 24 or 32 bytes
// blockSize  - The block size in bytes of this Rijndael (16, 24 or 32 bytes).
void CRijndael_MakeKey( CRijndael *ctx, char const* key, char const* chain, int keylength, int blockSize );
//Encrypt exactly one block of plaintext.
// in           - The plaintext.
// result       - The ciphertext generated from a plaintext using the key.
void CRijndael_EncryptBlock( CRijndael *ctx, char const* in, char* result );

//Decrypt exactly one block of ciphertext.
// in         - The ciphertext.
// result     - The plaintext generated from a ciphertext using the session key.
void CRijndael_DecryptBlock( CRijndael *ctx, char const* in, char* result );

extern char const* sm_chain0;

#endif // __RIJNDAEL_H__
