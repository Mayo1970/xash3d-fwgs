/*
ClientWindow.cpp - base client menu window
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

#include "BaseMenu.h"
#include "ClientWindow.h"

void CClientWindow::VidInit()
{
	size.w = 1024 - 100;
	size.h = 768 - 100;
	pos.x = ( ( ScreenWidth - uiStatic.scaleX * 1024 ) / 2 ) / uiStatic.scaleX + 50;
	pos.y = 50;

	BaseClass::VidInit();
	roundCornerSize = Size( 16, 16 ).Scale();

	iTitleHeight = 96 * uiStatic.scaleY;
	iGap = 2 * uiStatic.scaleY > 1 ? 4 * uiStatic.scaleY : 1;
}

void CClientWindow::Draw()
{
	UI_DrawPic( m_scPos, roundCornerSize, uiColorBlack, "gfx/vgui/round_corner_nw.tga", QM_DRAWTRANS );
	UI_DrawPic( m_scPos + Size( m_scSize.w - roundCornerSize.w, 0 ), roundCornerSize, uiColorBlack, "gfx/vgui/round_corner_ne.tga", QM_DRAWTRANS );
	UI_DrawPic( m_scPos + Size( 0, m_scSize.h - roundCornerSize.h ), roundCornerSize, uiColorBlack, "gfx/vgui/round_corner_sw.tga", QM_DRAWTRANS );
	UI_DrawPic( m_scPos + ( m_scSize - roundCornerSize ), roundCornerSize, uiColorBlack, "gfx/vgui/round_corner_se.tga", QM_DRAWTRANS );

	UI_FillRect( m_scPos + Size( roundCornerSize.w, 0 ), Size( m_scSize.w - roundCornerSize.w * 2, roundCornerSize.h ), uiColorBlack );
	UI_FillRect( m_scPos + Size( 0, roundCornerSize.h ), Size( m_scSize.w, iTitleHeight - roundCornerSize.h ), uiColorBlack );

	UI_FillRect( m_scPos + Size( 0, iTitleHeight + iGap ), Size( m_scSize.w, m_scSize.h - roundCornerSize.h - iTitleHeight - iGap ), uiColorBlack );
	UI_FillRect( m_scPos + Size( roundCornerSize.w, m_scSize.h - roundCornerSize.h ), Size( m_scSize.w - roundCornerSize.w * 2, roundCornerSize.h ), uiColorBlack );

	UI_DrawPic( m_scPos + roundCornerSize, roundCornerSize * 4, PackAlpha( uiPromptTextColor, 255 ), "gfx/vgui/CS_logo.tga", QM_DRAWTRANS );

	UI_DrawString( font, m_scPos + Size( roundCornerSize.w * 6, roundCornerSize.h ), Size( m_scSize.w - roundCornerSize.w * 6, roundCornerSize.h * 4 ),
	               szName, PackAlpha( uiPromptTextColor, 255 ), m_scChSize, QM_LEFT, ETF_NOSIZELIMIT );

	BaseClass::Draw();
}

bool CClientWindow::KeyDown( int key )
{
	if ( eTransitionType == ANIM_CLOSING )
		return false;

	if ( key == K_ENTER || key == K_KP_ENTER )
	{
		CMenuBaseItem *item = ItemAtCursor();
		if ( item && !( item->iFlags & ( QMF_GRAYED | QMF_INACTIVE ) ) )
		{
			item->_Event( QM_PRESSED );
			item->_Event( QM_RELEASED );
			return true;
		}
	}

	if ( key >= 0 && key < 256 && ( keys[key] || keyButtons[key] ) )
	{
		CMenuBaseItem *item = keyButtons[key] ? keyButtons[key] : ( m_iNumBtns ? buttons[0] : NULL );
		if ( item )
		{
			if ( keyButtons[key] )
			{
				static_cast<CMenuAction *>( keyButtons[key] )->onPressed( item );
			}
			else
			{
				( keys[key] )( item );
			}
			return true;
		}
	}

	return BaseClass::KeyDown( key );
}

bool CClientWindow::KeyUp( int key )
{
	return BaseClass::KeyUp( key );
}

CMenuAction *CClientWindow::AddButton( int key, const char *name, Point pos, CEventCallback callback )
{
	CMenuAction *act = new CMenuAction();

	act->pos = pos;
	act->onPressed = callback;
	act->SetBackground( 0U, PackRGBA( 255, 0, 0, 64 ) );
	if ( *name == '&' ) // fast hack
		name++;
	act->SetText( name );
	act->SetCharSize( QM_SMALLERFONT );
	act->size = Size( 250, 32 );
	act->bDrawStroke = true;
	act->m_bLimitBySize = true;
	act->colorStroke = uiInputTextColor;
	act->iStrokeWidth = 1;
	act->eFocusAnimation = QM_HIGHLIGHTIFFOCUS;
	act->eTextAlignment = QM_LEFT;
	act->SetTextInsetX( 8 );

	if ( key > 0 && key < 256 )
	{
		keys[key] = callback;
		keyButtons[key] = act;

		if ( key >= 'a' && key <= 'z' )
		{
			int upper = key - 'a' + 'A';
			keys[upper] = callback;
			keyButtons[upper] = act;
		}
		else if ( key >= 'A' && key <= 'Z' )
		{
			int lower = key - 'A' + 'a';
			keys[lower] = callback;
			keyButtons[lower] = act;
		}
	}

	if ( m_iNumBtns >= 256 )
	{
		Host_Error( "Too many client window buttons!" );
	}

	buttons[m_iNumBtns++] = act;

	AddItem( act );

	return act;
}
