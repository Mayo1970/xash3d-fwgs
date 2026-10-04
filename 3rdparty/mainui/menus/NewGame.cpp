// Copyright (C) 1997-2001 Id Software, Inc.
// GPLv2 or later, WITHOUT ANY WARRANTY; see the GNU General Public License.

#include "CheckBox.h"
#include "EventSystem.h"
#include "Framework.h"
#include "Bitmap.h"
#include "PicButton.h"
#include "YesNoMessageBox.h"
#include "keydefs.h"
#include "MenuStrings.h"

#define ART_BANNER		"gfx/shell/head_newgame"

class CMenuNewGame : public CMenuFramework
{
public:
	CMenuNewGame() : CMenuFramework( "CMenuNewGame" ) { }
	void StartGame( uintptr_t skill );
	void Show() override
	{
		if( gMenu.m_gameinfo.flags & GFL_NOSKILLS )
		{
			StartGame( 1.0f );
			return;
		}

		CMenuFramework::Show();
	}

private:
	void _Init() override;

	static void StartGameCb( CMenuBaseItem *pSelf, void *pExtra );
	static void ShowDialogCb( CMenuBaseItem *pSelf, void *pExtra  );

	CMenuYesNoMessageBox  msgBox;

	CEventCallback easyCallback;
	CEventCallback normCallback;
	CEventCallback hardCallback;

	CMenuCheckBox startDemoChapter;
};

void CMenuNewGame::StartGame( uintptr_t skill )
{
	if( EngFuncs::GetCvarFloat( "host_serverstate" ) && EngFuncs::GetCvarFloat( "maxplayers" ) > 1 )
		EngFuncs::HostEndGame( "end of the game" );

	EngFuncs::CvarSetValue( "skill", skill );
	EngFuncs::CvarSetValue( "deathmatch", 0.0f );
	EngFuncs::CvarSetValue( "teamplay", 0.0f );
	EngFuncs::CvarSetValue( "pausable", 1.0f ); // singleplayer is always allowing pause
	EngFuncs::CvarSetValue( "maxplayers", 1.0f );
	EngFuncs::CvarSetValue( "coop", 0.0f );

	EngFuncs::PlayBackgroundTrack( NULL, NULL );

	// Decay has no single-player mode: its partner bot needs a second slot. Same three-frame
	// wait as Create Server so maxplayers latches before the map starts.
	if( !stricmp( gMenu.m_gameinfo.gamefolder, "decay" ))
	{
		char map[256], cmd[512];

		Com_EscapeCommand( map, startDemoChapter.bChecked ? gMenu.m_gameinfo.demomap : gMenu.m_gameinfo.startmap, sizeof( map ));
		snprintf( cmd, sizeof( cmd ), "disconnect;menu_connectionprogress localserver;wait;wait;wait;maxplayers 2;latch;map %s\n", map );
		EngFuncs::ClientCmd( false, cmd );
		return;
	}

	if( startDemoChapter.bChecked )
		EngFuncs::ClientCmdF( false, "newgame \"%s\"\n", gMenu.m_gameinfo.demomap );
	else EngFuncs::ClientCmd( false, "newgame\n" );
}

void CMenuNewGame::StartGameCb( CMenuBaseItem *pSelf, void *pExtra )
{
	CMenuNewGame *ui = (CMenuNewGame*)pSelf->Parent();

	ui->StartGame( (uintptr_t)pExtra );
}

void CMenuNewGame::ShowDialogCb( CMenuBaseItem *pSelf, void *pExtra )
{
	CMenuNewGame *ui = (CMenuNewGame*)pSelf->Parent();

	ui->msgBox.onPositive = *(CEventCallback*)pExtra;
	ui->msgBox.Show();
}

void CMenuNewGame::_Init( void )
{
	AddItem( banner );

	banner.SetPicture( ART_BANNER );

	easyCallback = StartGameCb;
	easyCallback.pExtra = (void *)1;

	normCallback = StartGameCb;
	normCallback.pExtra = (void *)2;

	hardCallback = StartGameCb;
	hardCallback.pExtra = (void *)3;

	CMenuPicButton *easy = AddButton( L( "GameUI_Easy" ), L( "StringsList_200" ), PC_EASY, easyCallback, QMF_NOTIFY );
	CMenuPicButton *norm = AddButton( L( "GameUI_Medium" ), L( "StringsList_201" ), PC_MEDIUM, normCallback, QMF_NOTIFY );
	CMenuPicButton *hard = AddButton( L( "GameUI_Hard" ), L( "StringsList_202" ), PC_DIFFICULT, hardCallback, QMF_NOTIFY );

	easy->onReleasedClActive =
		norm->onReleasedClActive =
		hard->onReleasedClActive = ShowDialogCb;
	easy->onReleasedClActive.pExtra = &easyCallback;
	norm->onReleasedClActive.pExtra = &normCallback;
	hard->onReleasedClActive.pExtra = &hardCallback;

	AddButton( L( "GameUI_Cancel" ), L( "Go back to the Main menu" ), PC_CANCEL, VoidCb( &CMenuNewGame::Hide ), QMF_NOTIFY );

	startDemoChapter.SetCoord( 72, 230 + m_iBtnsNum * 50 );
	startDemoChapter.SetNameAndStatus( L( "GameUI_PlayGame_Alt" ), L( "Play the demo chapter on selected difficulty" ));

	if( EngFuncs::IsMapValid( gMenu.m_gameinfo.demomap ))
		AddItem( startDemoChapter );

	msgBox.SetMessage( L( "StringsList_240" ) );
	msgBox.HighlightChoice( CMenuYesNoMessageBox::HIGHLIGHT_NO );
	msgBox.Link( this );

}

ADD_MENU( menu_newgame, CMenuNewGame, UI_NewGame_Menu );
