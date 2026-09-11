// TFC-6 Phase 3: projectile weapons -- nails, rockets, IC, GL/pipebombs, flames.
// Every value is [tfc.so]: read out of the retail TFC server binary (DWARF + disasm).

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "soundent.h"
#include "decals.h"
#include "tf_defs.h"

extern DLL_GLOBAL short g_sModelIndexPlayerFlame;

#define TF_MDL_ROCKET       "models/rpgrocket.mdl"   // also the (invisible) nail and flame burst
#define TF_MDL_PIPEBOMB     "models/pipebomb.mdl"
#define TF_SPR_TRAIL        "sprites/smoke.spr"
#define TF_SND_ROCKET       "weapons/rocket1.wav"

#define TF_NAIL_SPEED       1000.0f
#define TF_NAIL_FAST        1500.0f  // tranq dart + railgun slug
#define TF_NAIL_LIFE        6.0f
#define TF_NAIL_DMG         9
#define TF_SUPERNAIL_DMG    13
#define TF_TRANQ_DMG        20.0f
#define TF_TRANQ_TIME       15.0f
#define TF_RAIL_DMG         25.0f

#define TF_RPG_SPEED        900.0f
#define TF_IC_SPEED         600.0f
#define TF_ROCKET_LIFE      5.0f
#define TF_RPG_DMG          92.0f    // + RANDOM_FLOAT( 0, 20 ) on impact
#define TF_IC_DMG           10.0f    // + RANDOM_FLOAT( 0, 20 ) on impact
#define TF_IC_BLAST         15.0f    // flat (DMG_RADIUS_MAX) inside TF_IC_RADIUS
#define TF_IC_RADIUS        180.0f

#define TF_GL_SPEED         600.0f
#define TF_GL_UP            200.0f
#define TF_GL_FUSE          2.5f
#define TF_GL_DMG           120
#define TF_PIPE_LIFE        120.0f
#define TF_PIPE_ARM         0.6f     // detpipe ignores pipes younger than this
#define TF_PIPE_MAX         8        // per player; the ninth sets off the oldest

#define TF_BURST_SPEED      600.0f
#define TF_BURST_LIFE       1.0f
#define TF_BURST_DMG        15.0f

#define TF_IGNITE_DMG       6.0f
#define TF_FLAME_LIFE       5.0f
#define TF_FLAME_MAX        4        // numflames cap; each point is 2 burn damage a second

static int g_iTFWorldFlames;         // [tfc.so] num_world_flames, bookkeeping only

// Rockets aim along v_angle: store the model pitch inverted, as a missile
// needs, and fly down the un-inverted forward vector.
static void TF_LaunchAlongAngles( entvars_t *pev, float flSpeed )
{
	pev->angles.x = -pev->angles.x;
	UTIL_MakeAimVectors( pev->angles );
	pev->velocity = gpGlobals->v_forward * flSpeed;
}

static void TF_BeamFollow( CBaseEntity *pEnt, int iSprite, int life, int width, int r, int g, int b, int bright )
{
	MESSAGE_BEGIN( MSG_BROADCAST, SVC_TEMPENTITY );
		WRITE_BYTE( TE_BEAMFOLLOW );
		WRITE_SHORT( pEnt->entindex() );
		WRITE_SHORT( iSprite );
		WRITE_BYTE( life );
		WRITE_BYTE( width );
		WRITE_BYTE( r );
		WRITE_BYTE( g );
		WRITE_BYTE( b );
		WRITE_BYTE( bright );
	MESSAGE_END();
}

// [tfc.so] UTIL_ParametricRocket: lets clients draw the rocket along its
// straight path instead of waiting on server position updates.
static void TF_ParametricRocket( entvars_t *pev, Vector vecOrigin, Vector vecAngles, edict_t *owner )
{
	TraceResult tr;

	pev->startpos = vecOrigin;
	UTIL_MakeVectors( vecAngles );
	UTIL_TraceLine( pev->startpos, pev->startpos + gpGlobals->v_forward * 8192.0f, ignore_monsters, owner, &tr );
	pev->endpos = tr.vecEndPos;

	float flSpeed = pev->velocity.Length();
	float flTravel = ( flSpeed > 0.0f ) ? ( pev->endpos - pev->startpos ).Length() / flSpeed : 0.0f;
	pev->impacttime = gpGlobals->time + flTravel;
	pev->starttime = gpGlobals->time;
}

static entvars_t *TF_OwnerVars( CBaseEntity *pEnt )
{
	return pEnt->pev->owner ? VARS( pEnt->pev->owner ) : NULL;
}

// [tfc.so] CGrenade::ExplodeTouch. The victim takes the full pev->dmg first and
// pev->enemy keeps them out of the splash that follows (see ::RadiusDamage).
static void TF_ProjDirectHit( CBaseEntity *pProj, CBaseEntity *pOther, TraceResult *ptr )
{
	entvars_t *pev = pProj->pev;
	Vector vecDir = pev->velocity.Normalize();
	Vector vecSpot = pev->origin - vecDir * 32.0f;

	pev->enemy = pOther->edict();
	UTIL_TraceLine( vecSpot, vecSpot + vecDir * 64.0f, ignore_monsters, ENT( pev ), ptr );

	if ( pev->owner && pOther->pev->takedamage != DAMAGE_NO )
		pOther->TakeDamage( pev, VARS( pev->owner ), pev->dmg, DMG_BLAST );
}

// [tfc.so] CGrenade::Explode: 32u pull-out, still-in-solid and owner-changed-team
// bails (returns FALSE, removed), then a client event or the stock sprite/decal/debris.
static BOOL TF_ProjExplode( CBaseEntity *pProj, TraceResult *pTrace, unsigned short usExplode, BOOL bNoSpark,
                            float flScale, float flDmg, float flRadius, int bitsDamageType )
{
	entvars_t *pev = pProj->pev;
	Vector vecStart = pev->origin;

	pev->model = iStringNull;
	pev->solid = SOLID_NOT;
	pev->takedamage = DAMAGE_NO;

	if ( pTrace->flFraction != 1.0f )
		pev->origin = pTrace->vecEndPos + pTrace->vecPlaneNormal * 32.0f;

	int iContents = UTIL_PointContents( pev->origin );
	if ( iContents == CONTENTS_SOLID )
	{
		pev->origin = vecStart;
		iContents = UTIL_PointContents( pev->origin );
		if ( iContents == CONTENTS_SOLID )
		{
			UTIL_Remove( pProj );
			return FALSE;
		}
	}

	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	int iOwnerTeam = pOwner ? pOwner->team_no : 0;
	if ( iOwnerTeam != pProj->team_no && pProj->team_no != 0 )
	{
		UTIL_Remove( pProj );
		return FALSE;
	}

	if ( usExplode )
	{
		PLAYBACK_EVENT_FULL( 0, NULL, usExplode, 0.0f, (float *)&vecStart, (float *)&g_vecZero,
		                     0.0f, 0.0f, 0, 0, 0, 0 );
	}
	else
	{
		MESSAGE_BEGIN( MSG_PAS, SVC_TEMPENTITY, pev->origin );
			WRITE_BYTE( TE_EXPLOSION );
			WRITE_COORD( pev->origin.x );
			WRITE_COORD( pev->origin.y );
			WRITE_COORD( pev->origin.z );
			WRITE_SHORT( g_sModelIndexFireball );
			WRITE_BYTE( (int)flScale );
			WRITE_BYTE( 15 );
			WRITE_BYTE( TE_EXPLFLAG_NOADDITIVE | TE_EXPLFLAG_NODLIGHTS );
		MESSAGE_END();
	}

	CSoundEnt::InsertSound( bits_SOUND_COMBAT, pev->origin, NORMAL_EXPLOSION_VOLUME, 3.0f );

	// the blast trace has to be able to reach whoever fired it
	entvars_t *pevOwner = TF_OwnerVars( pProj );
	pev->owner = NULL;
	::RadiusDamage( pev->origin, pev, pevOwner, flDmg, flRadius, CLASS_NONE, bitsDamageType );

	pev->effects |= EF_NODRAW;
	pev->velocity = g_vecZero;
	pProj->SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + 0.3f;

	if ( usExplode )
		return TRUE;

	UTIL_DecalTrace( pTrace, RANDOM_FLOAT( 0, 1 ) < 0.5f ? DECAL_SCORCH1 : DECAL_SCORCH2 );

	static const char *s_Debris[] = { "weapons/debris1.wav", "weapons/debris2.wav", "weapons/debris3.wav" };
	EMIT_SOUND_DYN( ENT( pev ), CHAN_VOICE, s_Debris[RANDOM_LONG( 0, 2 )], 0.55f, 0.8f, 0, PITCH_NORM );

	if ( iContents != CONTENTS_WATER && !bNoSpark )
	{
		int iSparks = RANDOM_LONG( 0, 3 );
		for ( int i = 0; i < iSparks; i++ )
			CBaseEntity::Create( "spark_shower", pev->origin, pTrace->vecPlaneNormal, NULL );
	}
	return TRUE;
}

// Nails (nailgun, super nailgun, tranq dart, railgun slug) are invisible on the
// server -- the weapon event draws them client-side.
LINK_ENTITY_TO_CLASS( tf_nailgun_nail, CTFNailgunNail )

void CTFNailgunNail::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_ROCKET );
	PRECACHE_SOUND( TF_SND_ROCKET );
}

void CTFNailgunNail::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_FLYMISSILE;
	pev->solid = SOLID_BBOX;
	SET_MODEL( ENT( pev ), TF_MDL_ROCKET );
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	UTIL_SetOrigin( pev, pev->origin );
	pev->classname = MAKE_STRING( "tf_nailgun_nail" );
	deathtype = MAKE_STRING( "nails" );

	UTIL_MakeVectors( pev->angles );
	pev->velocity = gpGlobals->v_forward * TF_NAIL_SPEED;
	pev->gravity = 0.5f;

	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + TF_NAIL_LIFE;
}

// Angles are v_angle, which MakeVectors reads directly. Vectors by value: a
// const& bound to gpGlobals->v_forward would change under Create().
CTFNailgunNail *CTFNailgunNail::CreateNail( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner, BOOL bNoDraw )
{
	CTFNailgunNail *pNail = (CTFNailgunNail *)CBaseEntity::Create( "tf_nailgun_nail", vecOrigin, vecAngles, pOwner->edict() );
	if ( !pNail )
		return NULL;

	pNail->SetTouch( &CTFNailgunNail::NailTouch );
	pNail->pev->dmg = TF_NAIL_DMG;
	if ( bNoDraw )
		pNail->pev->effects |= EF_NODRAW;
	return pNail;
}

CTFNailgunNail *CTFNailgunNail::CreateSuperNail( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner )
{
	CTFNailgunNail *pNail = CreateNail( vecOrigin, vecAngles, pOwner, TRUE );
	if ( !pNail )
		return NULL;

	pNail->deathtype = MAKE_STRING( "supernails" );
	pNail->pev->dmg = TF_SUPERNAIL_DMG;
	return pNail;
}

CTFNailgunNail *CTFNailgunNail::CreateTranqNail( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner )
{
	CTFNailgunNail *pNail = CreateNail( vecOrigin, vecAngles, pOwner, TRUE );
	if ( !pNail )
		return NULL;

	UTIL_MakeVectors( pNail->pev->angles );
	pNail->pev->velocity = gpGlobals->v_forward * TF_NAIL_FAST;
	pNail->SetTouch( &CTFNailgunNail::TranqTouch );
	return pNail;
}

CTFNailgunNail *CTFNailgunNail::CreateRailgunNail( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner )
{
	CTFNailgunNail *pNail = CreateNail( vecOrigin, vecAngles, pOwner, TRUE );
	if ( !pNail )
		return NULL;

	pNail->deathtype = MAKE_STRING( "railgun" );
	pNail->SetTouch( &CTFNailgunNail::RailgunNailTouch );
	UTIL_MakeVectors( vecAngles );
	pNail->pev->velocity = gpGlobals->v_forward * TF_NAIL_FAST;
	return pNail;
}

static CBaseEntity *TF_NailAttacker( CBaseEntity *pNail )
{
	return CBaseEntity::Instance( pNail->pev->owner ? pNail->pev->owner : INDEXENT( 0 ) );
}

// UTIL_Remove, not TFC's SUB_Remove: freeing the edict mid-touch is unsafe here.
// It only flags the edict, so go non-solid first or a second touch lands a second hit.
void CTFNailgunNail::NailTouch( CBaseEntity *pOther )
{
	// nails fly through each other
	BOOL bStop = ( pOther->pev->modelindex != pev->modelindex );
	if ( bStop )
	{
		pev->solid = SOLID_NOT;
		SetTouch( NULL );
	}

	if ( pOther->pev->takedamage != DAMAGE_NO )
	{
		if ( pOther->TakeDamage( pev, TF_OwnerVars( this ), pev->dmg, DMG_NAIL ) )
			SpawnBlood( pev->origin, pOther->BloodColor(), pev->dmg );
	}

	if ( bStop )
		UTIL_Remove( this );
}

void CTFNailgunNail::TranqTouch( CBaseEntity *pOther )
{
	pev->solid = SOLID_NOT;
	SetTouch( NULL );

	if ( pOther->pev->takedamage != DAMAGE_NO )
	{
		CBaseEntity *pAttacker = TF_NailAttacker( this );

		// a friendly dart only sedates when team damage is on at all
		BOOL bTranq = pOther->Classify() == CLASS_PLAYER;
		if ( bTranq && pAttacker->IsAlly( pOther ) && ( (int)gpGlobals->teamplay & TEAMPLAY_TEAMDAMAGE ) )
			bTranq = FALSE;

		if ( bTranq )
		{
			CBasePlayer *pl = (CBasePlayer *)pOther;
			if ( pl->tfstate & TFSTATE_TRANQUILISED )
			{
				CBaseEntity *pTimer = pl->FindTimer( TF_TIMER_TRANQUILISATION );
				if ( pTimer )
					pTimer->pev->nextthink = gpGlobals->time + TF_TRANQ_TIME;
			}
			else
			{
				ClientPrint( pl->pev, HUD_PRINTNOTIFY, "#Spy_tranq" );
				pl->tfstate |= TFSTATE_TRANQUILISED;

				CBaseEntity *pTimer = pl->CreateTimer( TF_TIMER_TRANQUILISATION );
				if ( pTimer )
				{
					pTimer->pev->nextthink = gpGlobals->time + TF_TRANQ_TIME;
					pTimer->SetThink( &CBaseEntity::Timer_Tranquilisation );
					pTimer->team_no = pAttacker->team_no;
				}
				pl->TeamFortress_SetSpeed();
			}
		}

		pOther->TakeDamage( pev, pAttacker->pev, TF_TRANQ_DMG, DMG_NAIL | DMG_TRANQ );
	}

	UTIL_Remove( this );
}

void CTFNailgunNail::RailgunNailTouch( CBaseEntity *pOther )
{
	pev->solid = SOLID_NOT;
	SetTouch( NULL );

	if ( pOther->pev->takedamage != DAMAGE_NO )
	{
		if ( pOther->TakeDamage( pev, TF_NailAttacker( this )->pev, TF_RAIL_DMG, DMG_ENERGYBEAM ) )
			SpawnBlood( pev->origin, pOther->BloodColor(), pev->dmg );
	}

	UTIL_Remove( this );
}

// Soldier rocket
LINK_ENTITY_TO_CLASS( tf_rpg_rocket, CTFRpgRocket )

void CTFRpgRocket::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_ROCKET );
	m_iTrail = PRECACHE_MODEL( TF_SPR_TRAIL );
	PRECACHE_SOUND( TF_SND_ROCKET );
}

void CTFRpgRocket::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_FLYMISSILE;
	pev->solid = SOLID_BBOX;
	SET_MODEL( ENT( pev ), TF_MDL_ROCKET );
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	UTIL_SetOrigin( pev, pev->origin );
	pev->classname = MAKE_STRING( "tf_rpg_rocket" );
	deathtype = MAKE_STRING( "rocket" );

	TF_LaunchAlongAngles( pev, TF_RPG_SPEED );
	pev->gravity = 0.5f;

	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + TF_ROCKET_LIFE;
}

CTFRpgRocket *CTFRpgRocket::CreateRpgRocket( Vector p_vecOrigin, Vector p_vecAngles, CBaseEntity *pOwner, CTFRpg *pLauncher )
{
	CTFRpgRocket *pRocket = (CTFRpgRocket *)CBaseEntity::Create( "tf_rpg_rocket", p_vecOrigin, p_vecAngles, pOwner->edict() );
	if ( !pRocket )
		return NULL;

	pRocket->SetTouch( &CTFRpgRocket::RocketTouch );
	pRocket->pev->effects |= EF_LIGHT;
	TF_BeamFollow( pRocket, pRocket->m_iTrail, 10, 5, 224, 224, 255, 128 );
	TF_ParametricRocket( pRocket->pev, p_vecOrigin, p_vecAngles, pOwner->edict() );
	return pRocket;
}

void CTFRpgRocket::RocketTouch( CBaseEntity *pOther )
{
	TraceResult tr;

	pev->dmg = TF_RPG_DMG + RANDOM_FLOAT( 0, 20 );
	STOP_SOUND( edict(), CHAN_VOICE, TF_SND_ROCKET );

	TF_ProjDirectHit( this, pOther, &tr );
	TF_ProjExplode( this, &tr, 0, TRUE, ( pev->dmg - 50.0f ) * 0.6f,
	                pev->dmg, pev->dmg, DMG_BLAST | DMG_RADIUS_QUAKE );
}

// Pyro incendiary cannon rocket
LINK_ENTITY_TO_CLASS( tf_ic_rocket, CTFIncendiaryCRocket )

void CTFIncendiaryCRocket::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_ROCKET );
	m_iTrail = PRECACHE_MODEL( TF_SPR_TRAIL );
	PRECACHE_SOUND( TF_SND_ROCKET );
	PRECACHE_SOUND( "weapons/sgun1.wav" );
	UTIL_PrecacheOther( "tf_flame" );
}

void CTFIncendiaryCRocket::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_FLYMISSILE;
	pev->solid = SOLID_BBOX;
	SET_MODEL( ENT( pev ), TF_MDL_ROCKET );
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	UTIL_SetOrigin( pev, pev->origin );
	pev->classname = MAKE_STRING( "tf_rpg_rocket" );   // [tfc.so] yes, the rocket classname
	deathtype = MAKE_STRING( "rocket" );

	TF_LaunchAlongAngles( pev, TF_IC_SPEED );
	pev->gravity = 0.5f;

	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + TF_ROCKET_LIFE;
}

CTFIncendiaryCRocket *CTFIncendiaryCRocket::CreateRpgRocket( Vector p_vecOrigin, Vector p_vecAngles, CBaseEntity *pOwner, CTFIncendiaryC *pLauncher )
{
	CTFIncendiaryCRocket *pRocket = (CTFIncendiaryCRocket *)CBaseEntity::Create( "tf_ic_rocket", p_vecOrigin, p_vecAngles, pOwner->edict() );
	if ( !pRocket )
		return NULL;

	pRocket->SetTouch( &CTFIncendiaryCRocket::RocketTouch );
	pRocket->pev->effects |= EF_LIGHT;
	TF_BeamFollow( pRocket, pRocket->m_iTrail, 30, 5, 224, 32, 32, 200 );
	TF_ParametricRocket( pRocket->pev, p_vecOrigin, p_vecAngles, pOwner->edict() );
	return pRocket;
}

// The victim is hit twice -- once here with the ignite bit, once by the shared
// direct-hit -- then everyone else in 180u takes a flat 15 and catches fire.
void CTFIncendiaryCRocket::RocketTouch( CBaseEntity *pOther )
{
	TraceResult tr;

	pev->dmg = TF_IC_DMG + RANDOM_FLOAT( 0, 20 );
	if ( pOther->pev->takedamage != DAMAGE_NO )
		pOther->TakeDamage( pev, TF_OwnerVars( this ), pev->dmg, DMG_BLAST | DMG_IGNITE );
	STOP_SOUND( edict(), CHAN_VOICE, TF_SND_ROCKET );

	TF_ProjDirectHit( this, pOther, &tr );
	if ( TF_ProjExplode( this, &tr, 0, TRUE, pev->dmg * 0.5f, TF_IC_BLAST, TF_IC_RADIUS,
	                     DMG_BLAST | DMG_IGNITE | DMG_RADIUS_MAX | DMG_WALLPIERCING ) )
		SetThink( &CTFIncendiaryCRocket::ICSmoke );
}

// [tfc.so] CGrenade::Smoke with the IC's own m_flSmokeScale = dmg + 30.
void CTFIncendiaryCRocket::ICSmoke( void )
{
	if ( UTIL_PointContents( pev->origin ) == CONTENTS_WATER )
	{
		UTIL_Bubbles( pev->origin - Vector( 64, 64, 64 ), pev->origin + Vector( 64, 64, 64 ), 100 );
	}
	else
	{
		MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pev->origin );
			WRITE_BYTE( TE_SMOKE );
			WRITE_COORD( pev->origin.x );
			WRITE_COORD( pev->origin.y );
			WRITE_COORD( pev->origin.z );
			WRITE_SHORT( g_sModelIndexSmoke );
			WRITE_BYTE( (int)( pev->dmg + 30.0f ) );
			WRITE_BYTE( 12 );
		MESSAGE_END();
	}
	UTIL_Remove( this );
}

// Demoman grenade launcher: GL grenades and pipebombs
LINK_ENTITY_TO_CLASS( tf_gl_grenade, CTFGrenade )

void CTFGrenade::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_ROCKET );
	PRECACHE_MODEL( TF_MDL_PIPEBOMB );
	m_iTrail = PRECACHE_MODEL( TF_SPR_TRAIL );
	PRECACHE_SOUND( TF_SND_ROCKET );
	m_usTFExplode = PRECACHE_EVENT( 1, "events/explode/tf_pipe.sc" );
}

void CTFGrenade::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_BOUNCE;
	pev->solid = SOLID_BBOX;
	pev->gravity = 1.0f;
	pev->friction = 0.5f;
	SET_MODEL( ENT( pev ), TF_MDL_PIPEBOMB );
	pev->skin = 1;   // GL grenade skin; pipebombs switch back to 0
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	UTIL_SetOrigin( pev, pev->origin );

	// draw order kept: up jitter first, then right
	UTIL_MakeVectors( pev->angles );
	float flUp = RANDOM_FLOAT( -10, 10 );
	float flRight = RANDOM_FLOAT( -10, 10 );
	pev->velocity = gpGlobals->v_forward * TF_GL_SPEED + gpGlobals->v_up * ( TF_GL_UP + flUp )
	              + gpGlobals->v_right * flRight;
	pev->avelocity = Vector( 300, 300, 300 );

	m_bQuiet = FALSE;
	pev->dmg = TF_GL_DMG;
	SetThink( &CTFGrenade::GLDetonate );
	pev->nextthink = gpGlobals->time + TF_GL_FUSE;
}

CTFGrenade *CTFGrenade::CreateTFGrenade( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner )
{
	CTFGrenade *pGren = (CTFGrenade *)CBaseEntity::Create( "tf_gl_grenade", vecOrigin, vecAngles, pOwner->edict() );
	if ( !pGren )
		return NULL;

	pGren->SetTouch( &CTFGrenade::GrenadeTouch );
	pGren->team_no = pOwner->team_no;
	pGren->deathtype = MAKE_STRING( "gl_grenade" );
	pGren->m_usTFExplode = PRECACHE_EVENT( 1, "events/explode/tf_gren.sc" );
	TF_BeamFollow( pGren, pGren->m_iTrail, 15, 5, 224, 224, 255, 80 );
	return pGren;
}

CTFGrenade *CTFGrenade::CreateTFPipebomb( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner )
{
	CTFGrenade *pGren = (CTFGrenade *)CBaseEntity::Create( "tf_gl_grenade", vecOrigin, vecAngles, pOwner->edict() );
	if ( !pGren )
		return NULL;

	pGren->SetTouch( &CTFGrenade::PipebombTouch );
	pGren->team_no = pOwner->team_no;
	pGren->pev->classname = MAKE_STRING( "tf_gl_pipebomb" );
	pGren->deathtype = MAKE_STRING( "pipebomb" );
	pGren->pev->skin = 0;
	pGren->m_flCreationTime = gpGlobals->time;
	pGren->SetThink( &CTFGrenade::PipebombDetonate );
	pGren->pev->nextthink = gpGlobals->time + TF_PIPE_LIFE;
	TF_BeamFollow( pGren, pGren->m_iTrail, 10, 5, 224, 224, 255, 80 );

	if ( pOwner->IsPlayer() )
	{
		CBasePlayer *pl = (CBasePlayer *)pOwner;
		if ( ++pl->m_iPipebombCount > TF_PIPE_MAX )
			pl->ExplodeOldPipebomb( FALSE, TRUE );
	}
	return pGren;
}

// Contact-detonates on anything that can be aimed at (players, buildings).
void CTFGrenade::GrenadeTouch( CBaseEntity *pOther )
{
	if ( pOther->pev->takedamage == DAMAGE_AIM )
	{
		TraceResult tr;
		TF_ProjDirectHit( this, pOther, &tr );
		TF_ProjExplode( this, &tr, m_usTFExplode, TRUE, 0.0f, pev->dmg, pev->dmg, DMG_BLAST | DMG_RADIUS_QUAKE );
		return;
	}

	BounceSound();
	if ( pev->flags & FL_ONGROUND )
	{
		pev->velocity = pev->velocity * 0.75f;
		if ( pev->velocity.Length() < 20.0f )
			pev->avelocity = g_vecZero;
	}
}

// [tfc.so] CGrenade::BounceTouch: damped on the ground, silent once it settles.
void CTFGrenade::PipebombTouch( CBaseEntity *pOther )
{
	if ( pOther->edict() == pev->owner )
		return;

	if ( pev->flags & FL_ONGROUND )
	{
		pev->velocity = pev->velocity * 0.6f;
		if ( pev->velocity.Length() <= 30.0f )
			m_bQuiet = TRUE;
	}
	else if ( !m_bQuiet )
	{
		BounceSound();
	}
}

static void TF_DownTrace( CBaseEntity *pEnt, TraceResult *ptr )
{
	Vector vecSpot = pEnt->pev->origin + Vector( 0, 0, 8 );
	UTIL_TraceLine( vecSpot, vecSpot + Vector( 0, 0, -32 ), ignore_monsters, pEnt->edict(), ptr );
}

void CTFGrenade::GLDetonate( void )
{
	TraceResult tr;
	TF_DownTrace( this, &tr );
	TF_ProjExplode( this, &tr, m_usTFExplode, TRUE, 0.0f, pev->dmg, pev->dmg, DMG_BLAST | DMG_RADIUS_QUAKE );
}

void CTFGrenade::PipebombDetonate( void )
{
	CBaseEntity *pOwner = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	if ( pOwner && pOwner->IsPlayer() )
		( (CBasePlayer *)pOwner )->m_iPipebombCount--;

	TraceResult tr;
	TF_DownTrace( this, &tr );
	TF_ProjExplode( this, &tr, m_usTFExplode, TRUE, 0.0f, pev->dmg, pev->dmg, DMG_BLAST | DMG_RADIUS_QUAKE );
}

// bAll: every pipe old enough to be armed (bForceDetonation: every pipe);
// otherwise just the oldest one. Detonation is a nextthink of now.
void CBasePlayer::ExplodeOldPipebomb( BOOL bAll, BOOL bForceDetonation )
{
	CTFGrenade *pOldest = NULL;
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "tf_gl_pipebomb" ) ) != NULL )
	{
		if ( pEnt->pev->owner != edict() )
			continue;

		CTFGrenade *pPipe = (CTFGrenade *)pEnt;
		if ( bAll )
		{
			if ( bForceDetonation || gpGlobals->time >= pPipe->m_flCreationTime + TF_PIPE_ARM )
				pPipe->pev->nextthink = gpGlobals->time;
		}
		else if ( !pOldest || pOldest->m_flCreationTime > pPipe->m_flCreationTime )
		{
			pOldest = pPipe;
		}
	}

	if ( pOldest )
		pOldest->pev->nextthink = gpGlobals->time;
}

void CBasePlayer::RemovePipebombs( void )
{
	CBaseEntity *pEnt = NULL;

	while ( ( pEnt = UTIL_FindEntityByClassname( pEnt, "tf_gl_pipebomb" ) ) != NULL )
	{
		if ( pEnt->pev->owner != edict() )
			continue;

		m_iPipebombCount--;
		pEnt->pev->flags |= FL_KILLME;
	}
}

// Pyro flamethrower burst
LINK_ENTITY_TO_CLASS( tf_flamethrower_burst, CTFFlamethrowerBurst )

void CTFFlamethrowerBurst::Precache( void )
{
	PRECACHE_MODEL( TF_MDL_ROCKET );
	PRECACHE_MODEL( TF_SPR_TRAIL );
	PRECACHE_SOUND( TF_SND_ROCKET );
	PRECACHE_MODEL( "sprites/fthrow.spr" );
}

void CTFFlamethrowerBurst::Spawn( void )
{
	Precache();
	pev->movetype = MOVETYPE_FLYMISSILE;
	pev->solid = SOLID_BBOX;
	SET_MODEL( ENT( pev ), TF_MDL_ROCKET );
	pev->effects |= EF_NODRAW;
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	UTIL_SetOrigin( pev, pev->origin );
	pev->classname = MAKE_STRING( "tf_flamethrower_burst" );
	deathtype = MAKE_STRING( "flames" );

	TF_LaunchAlongAngles( pev, TF_BURST_SPEED );
	pev->gravity = 0.5f;

	SetThink( &CBaseEntity::SUB_Remove );
	pev->nextthink = gpGlobals->time + TF_BURST_LIFE;
}

CTFFlamethrowerBurst *CTFFlamethrowerBurst::CreateBurst( Vector vecOrigin, Vector vecAngles, CBaseEntity *pOwner )
{
	CTFFlamethrowerBurst *pBurst = (CTFFlamethrowerBurst *)CBaseEntity::Create( "tf_flamethrower_burst", vecOrigin, vecAngles, pOwner->edict() );
	if ( !pBurst )
		return NULL;

	pBurst->SetTouch( &CTFFlamethrowerBurst::BurstTouch );
	return pBurst;
}

void CTFFlamethrowerBurst::BurstTouch( CBaseEntity *pOther )
{
	pev->solid = SOLID_NOT;
	SetTouch( NULL );

	if ( pOther->pev->takedamage != DAMAGE_NO )
		pOther->TakeDamage( pev, TF_OwnerVars( this ), TF_BURST_DMG, DMG_BURN | DMG_IGNITE );

	UTIL_Remove( this );
}

// Burning: CBasePlayer::Ignite + the CTFFlame that rides the victim
LINK_ENTITY_TO_CLASS( tf_flame, CTFFlame )

void CTFFlame::Precache( void )
{
	PRECACHE_SOUND( "ambience/fire1.wav" );
	PRECACHE_SOUND( "ambience/flameburst1.wav" );
	PRECACHE_SOUND( "ambience/steamburst1.wav" );
}

void CTFFlame::Spawn( void )
{
	Precache();
	pev->solid = SOLID_NOT;
	UTIL_SetSize( pev, g_vecZero, g_vecZero );
	UTIL_SetOrigin( pev, pev->origin );
	pev->classname = MAKE_STRING( "tf_fire" );
	deathtype = MAKE_STRING( "flames" );

	pev->health = gpGlobals->time + TF_FLAME_LIFE;   // [tfc.so] health doubles as the expiry time
	pev->effects |= EF_NODRAW | EF_DIMLIGHT;
	SetThink( &CTFFlame::FlameThink );
	pev->nextthink = gpGlobals->time + 1.0f;
	m_flNextDamageTime = pev->nextthink;
}

static void TF_PlayerFlameSprites( CBaseEntity *pVictim, int iVariance )
{
	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, pVictim->pev->origin );
		WRITE_BYTE( TE_PLAYERSPRITES );
		WRITE_SHORT( pVictim->entindex() );
		WRITE_SHORT( g_sModelIndexPlayerFlame );
		WRITE_BYTE( 5 );
		WRITE_BYTE( iVariance );
	MESSAGE_END();
}

CTFFlame *CTFFlame::FlameSpawn( CBaseEntity *pAttacker, CBaseEntity *pVictim )
{
	CTFFlame *pFlame = (CTFFlame *)CBaseEntity::Create( "tf_flame", pVictim->pev->origin, g_vecZero,
	                                                   pAttacker ? pAttacker->edict() : NULL );
	if ( !pFlame )
		return NULL;

	pFlame->pev->enemy = pVictim->edict();
	EMIT_SOUND_DYN( pFlame->edict(), CHAN_VOICE, "ambience/fire1.wav", 1.0f, 0.5f, 0, PITCH_NORM );
	TF_PlayerFlameSprites( pVictim, 25 );

	pFlame->pev->movetype = MOVETYPE_FOLLOW;
	pFlame->pev->aiment = pVictim->edict();
	g_iTFWorldFlames++;
	return pFlame;
}

void CTFFlame::FlameDestroy( void )
{
	STOP_SOUND( edict(), CHAN_VOICE, "ambience/fire1.wav" );
	g_iTFWorldFlames--;
	SUB_Remove();
}

void CTFFlame::FlameThink( void )
{
	CBaseEntity *pAttacker = pev->owner ? CBaseEntity::Instance( pev->owner ) : NULL;
	CBaseEntity *pVictim = pev->enemy ? CBaseEntity::Instance( pev->enemy ) : NULL;

	if ( !pVictim )
	{
		FlameDestroy();
		return;
	}

	TF_PlayerFlameSprites( pVictim, 50 );

	// put out (water, a medic) since the last tick
	if ( pVictim->numflames == 0.0f )
	{
		FlameDestroy();
		return;
	}

	// a burning corpse gives a last little blast
	if ( pVictim->pev->health < 1.0f )
	{
		::RadiusDamage( pev->origin, pev, pAttacker ? pAttacker->pev : pev, 10.0f, 10.0f, CLASS_NONE,
		                DMG_BURN | DMG_RADIUS_QUAKE );
		pVictim->numflames = 0;
		FlameDestroy();
		return;
	}

	if ( ( pVictim->armorclass & AT_SAVEFIRE ) && pVictim->pev->armorvalue > 0.0f )
	{
		pVictim->numflames -= 1.0f;
		FlameDestroy();
		return;
	}

	// re-lit while already burning: the fire lasts another full five seconds
	if ( pVictim->tfstate & TFSTATE_RESET_FLAMETIME )
	{
		pev->health = gpGlobals->time + TF_FLAME_LIFE;
		pVictim->tfstate &= ~TFSTATE_RESET_FLAMETIME;
	}

	if ( gpGlobals->time >= m_flNextDamageTime )
	{
		if ( gpGlobals->time > pev->health )
		{
			pVictim->numflames = 0;
			FlameDestroy();
			return;
		}

		if ( pVictim->IsAlive() )
			pVictim->TakeDamage( pev, pAttacker ? pAttacker->pev : NULL, pVictim->numflames * 2.0f, DMG_BURN );

		m_flNextDamageTime = gpGlobals->time + 1.0f;
	}

	pev->nextthink = gpGlobals->time + 0.2f;
}

// [tfc.so] CBasePlayer::Ignite. The first flame spawns the CTFFlame; later ones
// only stack numflames (up to TF_FLAME_MAX) and refresh its lifetime.
void CBasePlayer::Ignite( entvars_t *pevInflictor, entvars_t *pevAttacker )
{
	if ( IsAlive() )
		TakeDamage( pevInflictor, pevAttacker, TF_IGNITE_DMG, DMG_BURN );

	if ( cb_prematch_time > gpGlobals->time )
		return;

	if ( numflames >= TF_FLAME_MAX )
	{
		tfstate |= TFSTATE_RESET_FLAMETIME;
		return;
	}

	// asbestos armor (the pyro) cannot be set alight while it lasts
	if ( ( armorclass & AT_SAVEFIRE ) && pev->armorvalue > 0.0f )
		return;

	CBaseEntity *pAttacker = CBaseEntity::Instance( pevAttacker ? ENT( pevAttacker ) : INDEXENT( 0 ) );
	if ( ( (int)gpGlobals->teamplay & TEAMPLAY_NOEXPLOSIVE ) && pAttacker && IsAlly( pAttacker ) )
		return;

	numflames += 1.0f;
	ClientPrint( pev, HUD_PRINTCENTER, "#Pyro_onfire" );
	EMIT_SOUND_DYN( edict(), CHAN_VOICE, "ambience/flameburst1.wav", 1.0f, 0.8f, 0, PITCH_NORM );

	if ( numflames == 1.0f )
	{
		CTFFlame *pFlame = CTFFlame::FlameSpawn( pAttacker, this );
		if ( pFlame )
			pFlame->pev->effects &= ~EF_NODRAW;
	}
}

CLaserSpot *CLaserSpot::CreateSpot( void )
{
	return 0;
}
