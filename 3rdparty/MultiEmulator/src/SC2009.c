#include "multi_emulator.h"
#include "RevSpoofer.h"
#include "CRijndael.h"
#include "SHA.h"

int GenerateSC2009( void *pDest, const char *szXashID, int nSteamID )
{
	char hwid[64];

	strncpy( hwid, szXashID, 32 );
	hwid[32] = 0;
	if( !RevSpoofer_Spoof( hwid, nSteamID ))
		return 0;

	int *pTicket = (int *)pDest;
	unsigned char *pbTicket = (unsigned char *)pDest;

	unsigned int  revHash = RevSpoofer_Hash( hwid );

	pTicket[0] = 'S';     // +0
	pTicket[1] = revHash; // +4
	pTicket[2] = 'r' << 16 | 'e' << 8 | 'v';
	;                          // +8
	pTicket[3] = 0;            // +12
	pTicket[4] = revHash << 1; // +16
	pTicket[5] = 0x01100001;   // +20

	/* Encrypt HWID with AESKeyRand key and save it in the ticket. */
	static const char AESKeyRand[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
	char      AESHashRand[32];
	CRijndael AESRand;
	CRijndael_Init( &AESRand );
	CRijndael_MakeKey( &AESRand, AESKeyRand, sm_chain0, 32, 32 );
	CRijndael_EncryptBlock( &AESRand, hwid, AESHashRand );
	memcpy( &pbTicket[24], AESHashRand, 32 );

	/* Encrypt AESKeyRand with AESKeyRev key and save it in the ticket.
	 * AESKeyRev key is identical to the key in dproto/reunion. */
	static const char AESKeyRev[] = "_YOU_SERIOUSLY_NEED_TO_GET_LAID_";
	char      AESHashRev[32];
	CRijndael AESRev;
	CRijndael_Init( &AESRev );
	CRijndael_MakeKey( &AESRev, AESKeyRev, sm_chain0, 32, 32 );
	CRijndael_EncryptBlock( &AESRev, AESKeyRand, AESHashRev );
	memcpy( &pbTicket[56], AESHashRev, 32 );

	/* Perform HWID hashing and save hash to the ticket. */
	char SHAHash[32];
	CSHA sha;
	CSHA_Reset( &sha );
	CSHA_AddData( &sha, hwid, 32 );
	CSHA_FinalDigest( &sha, SHAHash );
	memcpy( &pbTicket[88], SHAHash, 32 );

	return 178;
}
