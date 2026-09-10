class CTeamFortress : public CHalfLifeTeamplay
{
public:
	CTeamFortress( void );

	virtual void Think( void );
	virtual BOOL IsTeamplay( void );
	virtual void PlayerSpawn( CBasePlayer *pPlayer );
	virtual BOOL ClientCommand( CBasePlayer *pPlayer, const char *pcmd );
	virtual void ClientUserInfoChanged( CBasePlayer *pPlayer, char *infobuffer );
	virtual int DeadPlayerWeapons( CBasePlayer *pPlayer );
	virtual int DeadPlayerAmmo( CBasePlayer *pPlayer );
	virtual float FlItemRespawnTime( CItem *pItem );
	virtual BOOL CanHaveItem( CBasePlayer *pPlayer, CItem *pItem );
	virtual const char *GetGameDescription( void );
	virtual const char *GetTeamID( CBaseEntity *pEntity );
	virtual void InitHUD( CBasePlayer *pl );
	virtual BOOL ClientConnected( edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128] );
	virtual void GoToIntermission( void );

	// TFC-6 Phase 1: real team/class model (base CHalfLifeTeamplay treats the
	// "model" userinfo -- a class name in TFC -- as the team name).
	virtual void PlayerThink( CBasePlayer *pPlayer );
	virtual edict_t *GetPlayerSpawnSpot( CBasePlayer *pPlayer );
	virtual const char *SetDefaultPlayerTeam( CBasePlayer *pPlayer );
	virtual int GetTeamIndex( const char *pTeamName );
	virtual const char *GetIndexedTeamName( int teamIndex );
	virtual BOOL IsValidTeam( const char *pTeamName );

#ifndef NO_VOICEGAMEMGR
	CVoiceGameMgr m_VoiceGameMgr;
#endif
};