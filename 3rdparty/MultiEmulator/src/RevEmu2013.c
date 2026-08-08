#include "multi_emulator.h"
#include "RevSpoofer.h"
#include "CRijndael.h"
#include "SHA.h"
#include <time.h>

int GenerateRevEmu2013( void *pDest, const char *szXashID, int nSteamID )
{
	char szhwid[64];

	strncpy( szhwid, szXashID, 32 );
	szhwid[32] = 0;
	if( !RevSpoofer_Spoof( szhwid, nSteamID ))
		return 0;

	int *pTicket = (int *)pDest;
	unsigned char *pbTicket = (unsigned char *)pDest;

	unsigned int  revHash = RevSpoofer_Hash( szhwid );

	pTicket[0] = 'S';     // +0
	pTicket[1] = revHash; // +4
	pTicket[2] = 'r' << 16 | 'e' << 8 | 'v';
	;                                    // +8
	pTicket[3] = 0;                      // +12
	pTicket[4] = revHash << 1;           // +16
	pTicket[5] = 0x01100001;             // +20
	pTicket[6] = (int)time( 0 ) + 90123; // +24
	pbTicket[27] = ~( pbTicket[27] + pbTicket[24] );
	pTicket[7] = ~(int)time( 0 );  // +28
	pTicket[8] = revHash * 2 >> 3; // +32
	pTicket[9] = 0;                // +36

	static const char c_szAESKeyRand[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";

	char      szAESHashRand[32];
	CRijndael AESRand;
	CRijndael_Init( &AESRand );
	CRijndael_MakeKey( &AESRand, c_szAESKeyRand, sm_chain0, 32, 32 );
	CRijndael_EncryptBlock( &AESRand, szhwid, szAESHashRand );
	memcpy( &pbTicket[40], szAESHashRand, 32 );

	static const char c_szAESKeyRev[] = "_YOU_SERIOUSLY_NEED_TO_GET_LAID_";
	char      AESHashRev[32];
	CRijndael AESRev;
	CRijndael_Init( &AESRev );
	CRijndael_MakeKey( &AESRev, c_szAESKeyRev, sm_chain0, 32, 32 );
	CRijndael_EncryptBlock( &AESRev, c_szAESKeyRand, AESHashRev );
	memcpy( &pbTicket[72], AESHashRev, 32 );

	char szSHAHash[32];
	CSHA sha;
	CSHA_Reset( &sha );
	CSHA_AddData( &sha, szhwid, 32 );
	CSHA_FinalDigest( &sha, szSHAHash );
	memcpy( &pbTicket[104], szSHAHash, 32 );

	return 194;
}
