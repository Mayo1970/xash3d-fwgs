/*
ClientWindow.h - base client menu window
Copyright (C) 2018 a1batross
Copyright (C) 2026 $_Vladislav

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#pragma once

#include "BaseClientWindow.h"
#include "Action.h"

class CClientWindow : public CMenuBaseClientWindow
{
public:
	typedef CMenuBaseClientWindow BaseClass;
	CClientWindow( const char *name = "CClientWindow" ) : BaseClass( name )
	{
		// SetCharSize( QM_DEFAULTFONT );
		m_iNumBtns = 0;
		for ( int i = 0; i < 256; i++ )
		{
			keys[i].Reset();
			keyButtons[i] = NULL;
		}
	}
	~CClientWindow()
	{
		for ( int i = 0; i < m_iNumBtns; i++ )
		{
			delete buttons[i];
		}
	}

	void Show() override
	{
		EngFuncs::KEY_SetDest( KEY_MENU );
		EngFuncs::ClientCmd( true, "touch_setclientonly 1" );
		BaseClass::Show();
	}
	void Hide() override
	{
		BaseClass::Hide();
		if ( m_pStack->Count() <= 1 )
		{
			EngFuncs::KEY_ClearStates();
			EngFuncs::KEY_SetDest( KEY_GAME );
			EngFuncs::ClientCmd( false, "touch_setclientonly 0" );
		}
	}

	CEventCallback ExecAndHide( const char *szCmd )
	{
		return CEventCallback( []( CMenuBaseItem *pSelf, void *pExtra )
		                       {
			UI_CloseClientMenu();
			EngFuncs::ClientCmd( false, (const char*)pExtra ); }, (void *)szCmd );
	}

	CMenuAction *AddButton( int key, const char *name, Point pos, CEventCallback callback );

	bool KeyUp( int key ) override;
	bool KeyDown( int key ) override;
	void VidInit() override;
	void Draw() override;
	CEventCallback keys[256];

protected:
	CMenuAction *buttons[256];
	CMenuBaseItem *keyButtons[256];
	int m_iNumBtns;

private:
	Size roundCornerSize;
	int iTitleHeight;
	int iGap;
};
