#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"

#include "tf_defs.h"

extern int gmsgTeamScore;

const char *sTNameCvars[] = {
	"team1",
	"team2",
	"team3",
	"team4",
	"t1",
	"t2",
	"t3",
	"t4",
	"Blue",
	"Red",
	"Yellow",
	"Green"
};

const char *GetTeamName( int tno )
{
	const char *result = "";

	if ( tno > 0 )
	{
		if ( team_names[tno] )
			result = STRING( team_names[tno] );
		else
			result = sTNameCvars[tno + 7];
	}

	return result;
}

void TeamFortress_TeamSetColor( int tno )
{
	// Blue
	teamcolors[1][PC_SCOUT] = { 153, 139 };
	teamcolors[1][PC_SNIPER] = { 153, 145 };
	teamcolors[1][PC_SOLDIER] = { 153, 130 };
	teamcolors[1][PC_DEMOMAN] = { 153, 145 };
	teamcolors[1][PC_MEDIC] = { 153, 140 };
	teamcolors[1][PC_HVYWEAP] = { 148, 138 };
	teamcolors[1][PC_PYRO] = { 140, 145 };
	teamcolors[1][PC_SPY] = { 150, 145 };
	teamcolors[1][PC_ENGINEER] = { 140, 148 };
	teamcolors[1][PC_RANDOM] = { 150, 0 };
	teamcolors[1][PC_CIVILIAN] = { 150, 140 };

	// Red
	teamcolors[2][PC_SCOUT] = { 255, 10 };
	teamcolors[2][PC_SNIPER] = { 255, 10 };
	teamcolors[2][PC_SOLDIER] = { 250, 58 };
	teamcolors[2][PC_DEMOMAN] = { 255, 20 };
	teamcolors[2][PC_MEDIC] = { 255, 250 };
	teamcolors[2][PC_HVYWEAP] = { 255, 25 };
	teamcolors[2][PC_PYRO] = { 250, 25 };
	teamcolors[2][PC_SPY] = { 250, 240 };
	teamcolors[2][PC_ENGINEER] = { 5, 250 };
	teamcolors[2][PC_RANDOM] = { 250, 0 };
	teamcolors[2][PC_CIVILIAN] = { 250, 240 };

	// Yellow
	teamcolors[3][PC_SCOUT] = { 45, 35 };
	teamcolors[3][PC_SNIPER] = { 45, 35 };
	teamcolors[3][PC_SOLDIER] = { 45, 35 };
	teamcolors[3][PC_DEMOMAN] = { 45, 35 };
	teamcolors[3][PC_MEDIC] = { 45, 35 };
	teamcolors[3][PC_HVYWEAP] = { 45, 40 };
	teamcolors[3][PC_PYRO] = { 45, 35 };
	teamcolors[3][PC_SPY] = { 45, 35 };
	teamcolors[3][PC_ENGINEER] = { 45, 5 };
	teamcolors[3][PC_RANDOM] = { 45, 0 };
	teamcolors[3][PC_CIVILIAN] = { 45, 35 };

	// Green
	teamcolors[4][PC_SCOUT] = { 100, 90 };
	teamcolors[4][PC_SNIPER] = { 80, 90 };
	teamcolors[4][PC_SOLDIER] = { 100, 40 };
	teamcolors[4][PC_DEMOMAN] = { 100, 90 };
	teamcolors[4][PC_MEDIC] = { 100, 90 };
	teamcolors[4][PC_HVYWEAP] = { 100, 90 };
	teamcolors[4][PC_PYRO] = { 100, 50 };
	teamcolors[4][PC_SPY] = { 100, 90 };
	teamcolors[4][PC_ENGINEER] = { 100, 90 };
	teamcolors[4][PC_RANDOM] = { 100, 0 };
	teamcolors[4][PC_CIVILIAN] = { 100, 90 };

	rgbcolors[0] = { 255.0f, 255.0f, 255.0f };
	rgbcolors[1] = { 0.0f, 0.0f, 255.0f };
	rgbcolors[2] = { 255.0f, 0.0f, 0.0f };
	rgbcolors[3] = { 255.0f, 255.0f, 30.0f };
	rgbcolors[4] = { 0.0f, 255.0f, 0.0f };
}

int TeamFortress_TeamGetNoPlayers( int tno )
{
	int n = 0;

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBaseEntity *pPlayer = UTIL_PlayerByIndex( i );
		if ( pPlayer && pPlayer->team_no == tno )
			n++;
	}

	return n;
}

int TeamFortress_TeamGetScoreFrags( int tno )
{
	return teamscores[tno];
}

// [tfc.so] slot 0 is civilian-only when its illegal-class mask is -1
BOOL TeamFortress_TeamIsCivilian( float tno )
{
	if ( tno == 1.0f ) return ( civilianteams & 1 ) != 0;
	if ( tno == 2.0f ) return ( civilianteams & 2 ) != 0;
	if ( tno == 3.0f ) return ( civilianteams & 4 ) != 0;
	if ( tno == 4.0f ) return ( civilianteams & 8 ) != 0;
	if ( tno == 0.0f ) return illegalclasses[0] == -1;
	return FALSE;
}

static const char *g_szTeamColors[5] = { "", "Blue", "Red", "Yellow", "Green" };

void TeamFortress_TeamShowScores( BOOL bLong, CBasePlayer *pPlayer )
{
	for ( int i = 1; (float)i <= number_of_teams; i++ )
	{
		const char *pszLine = bLong
			? UTIL_VarArgs( "Team %d (%s): %d\n", i, g_szTeamColors[i], teamscores[i] )
			: UTIL_VarArgs( "%s: %d\n", g_szTeamColors[i], teamscores[i] );

		if ( pPlayer )
			ClientPrint( pPlayer->pev, HUD_PRINTNOTIFY, pszLine );
		else
			UTIL_ClientPrintAll( HUD_PRINTNOTIFY, pszLine );
	}
}

// Velaron: TODO
BOOL TeamFortress_SortTeams( void )
{
	int bGotScore[5];

	memset( bGotScore, 0, sizeof( bGotScore ) );
	memset( g_iOrderedTeams, 0, sizeof( g_iOrderedTeams ) );

	if ( number_of_teams < 1.0f )
		return TRUE;

	// Velaron: TODO -- no sorting yet; the return only silences missing-return.
	return TRUE;
}

void TeamFortress_TeamIncreaseScore( int tno, int scoretoadd )
{
	if ( tno <= 0 || tno > 4 )
		return;
	
	teamscores[tno] += scoretoadd;

	MESSAGE_BEGIN( MSG_ALL, gmsgTeamScore );
	WRITE_STRING( GetTeamName( tno ) );
	WRITE_SHORT( teamscores[tno] );
	WRITE_SHORT( 0 );
	MESSAGE_END();
}

// Velaron: TODO
void CalculateTeamEqualiser( void )
{
	
}