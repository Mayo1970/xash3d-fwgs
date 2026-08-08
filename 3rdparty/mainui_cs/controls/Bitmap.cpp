/*
Bitmap.cpp - bitmap menu item
Copyright (C) 2010 Uncle Mike
Copyright (C) 2017 a1batross

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "extdll_menu.h"
#include "BaseMenu.h"
#include "Bitmap.h"
#include "PicButton.h" // GetTitleTransFraction
#include "Utils.h"
#include "BaseWindow.h"

CMenuBitmap::CMenuBitmap() : BaseClass()
{
	bKeepAspectRatio = false;
	SetRenderMode( QM_DRAWNORMAL );
}

/*
=================
CMenuBitmap::Init
=================
*/
void CMenuBitmap::VidInit( )
{
	colorBase.SetDefault( uiColorWhite );
	colorFocus.SetDefault( uiColorWhite );

	BaseClass::VidInit();
	if( !szFocusPic )
		szFocusPic = szPic;
}

bool CMenuBitmap::KeyUp( int key )
{
	const char *sound = 0;

	if( UI::Key::IsEnter( key ) && !(iFlags & QMF_MOUSEONLY) )
		sound = uiStatic.sounds[SND_LAUNCH];
	else if( UI::Key::IsLeftMouse( key ) && ( iFlags & QMF_HASMOUSEFOCUS ) )
		sound = uiStatic.sounds[SND_LAUNCH];

	if( sound )
	{
		_Event( QM_PRESSED );
		PlayLocalSound( sound );
	}

	return sound != NULL;
}

bool CMenuBitmap::KeyDown( int key )
{
	bool handled = false;

	if( UI::Key::IsEnter( key ) && !(iFlags & QMF_MOUSEONLY) )
		handled = true;
	else if( UI::Key::IsLeftMouse( key ) && ( iFlags & QMF_HASMOUSEFOCUS ) )
		handled = true;

	if( handled )
		_Event( QM_RELEASED );

	return handled;
}

/*
=================
CMenuBitmap::Draw
=================
*/
void CMenuBitmap::Draw( void )
{
	if( bDrawStroke )
		UI_DrawRectangle( m_scPos, m_scSize, colorStroke );

	if( !szPic.IsValid() )
	{
		if( colorBase != 0 )
			UI_FillRect( m_scPos, m_scSize, colorBase );
		return;
	}

	CImage *pPic = &szPic;
	ERenderMode mode = eRenderMode;
	unsigned int color = colorBase;
	bool hasFocus = false;

	if( iFlags & QMF_GRAYED )
	{
		color = uiColorDkGrey;
	}
	else if( ( iFlags & QMF_MOUSEONLY ) && !( iFlags & QMF_HASMOUSEFOCUS ) )
	{
		// no focus
	}
	else if( this == m_pParent->ItemAtCursor() )
	{
		hasFocus = true;
	}

	if( hasFocus )
	{
		pPic = &szFocusPic;
		mode = eFocusRenderMode;
		if( eFocusAnimation == QM_PULSEIFFOCUS )
		{
			color = PackAlpha( colorBase, 255 * (0.5f + 0.5f * sin( (float)uiStatic.realTime / UI_PULSE_DIVISOR )));
		}
	}
	else if( m_bPressed )
	{
		pPic = &szPressPic;
		mode = ePressRenderMode;
	}

	Point drawPos = m_scPos;
	Size drawSize = m_scSize;

	if( bKeepAspectRatio )
	{
		int w = EngFuncs::PIC_Width( *pPic );
		int h = EngFuncs::PIC_Height( *pPic );

		if( w > 0 && h > 0 )
		{
			Point offset;
			UI_FitSizePreserveAspect( Size( w, h ), m_scSize, offset, drawSize );
			drawPos.x += offset.x;
			drawPos.y += offset.y;
		}
	}

	UI_DrawPic( drawPos, drawSize, color, *pPic, mode );
}
