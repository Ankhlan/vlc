/*****************************************************************************
 * dj_deck.cpp : the DJ's deck, docked in the main window
 *****************************************************************************
 * Copyright (C) 2026 Ankhlan Erdenebat
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "components/dj_deck.hpp"
#include "../../../control/dj_host.h"

#include <QEvent>
#include <QWheelEvent>

#ifdef _WIN32
# include <windows.h>
#endif

DjDeck::DjDeck( intf_thread_t *_p_intf, QWidget *parent )
    : QWidget( parent ), p_intf( _p_intf ), host( NULL )
{
    /* a window of its own for the deck to be a child of, and nothing of Qt's painted over it */
    setAttribute( Qt::WA_NativeWindow );
    setAttribute( Qt::WA_DontCreateNativeAncestors );
    setAttribute( Qt::WA_PaintOnScreen );
    setAttribute( Qt::WA_NoSystemBackground );
    setContextMenuPolicy( Qt::PreventContextMenu );
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );

    var_Create( THEPL, DJ_HOST_VAR, VLC_VAR_ADDRESS );
    var_AddCallback( THEPL, DJ_HOST_VAR, HostChanged, this );
    /* once the main window stands: a window made now would be made again */
    QMetaObject::invokeMethod( this, "attach", Qt::QueuedConnection );
}

DjDeck::~DjDeck()
{
    var_DelCallback( THEPL, DJ_HOST_VAR, HostChanged, this );
    const dj_host_t *now = (const dj_host_t *)var_GetAddress( THEPL, DJ_HOST_VAR );
    if( now ) now->undock();   /* before Qt takes its window down, and the deck in it */
    var_Destroy( THEPL, DJ_HOST_VAR );
}

QSize DjDeck::sizeHint() const
{
    return QSize( 1100, 720 );
}

QSize DjDeck::minimumSizeHint() const
{
    return QSize( 1000, 640 );   /* less, and the records crowd the decks */
}

/* any thread: the DJ came or went */
int DjDeck::HostChanged( vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *data )
{
    QMetaObject::invokeMethod( static_cast<DjDeck *>( data ), "attach", Qt::QueuedConnection );
    return VLC_SUCCESS;
}

/* the deck's thread: someone asked for the deck */
void DjDeck::Show( void *data )
{
    emit static_cast<DjDeck *>( data )->wanted();
}

void DjDeck::attach()
{
    const dj_host_t *now = (const dj_host_t *)var_GetAddress( THEPL, DJ_HOST_VAR );
#ifdef _WIN32
    if( now && !now->dock( (void *)winId(), Show, this ) )
        now = NULL;
#else
    now = NULL;
#endif
    bool changed = (now != NULL) != (host != NULL);
    host = now;
    if( changed )
        emit availabilityChanged( host != NULL );
}

bool DjDeck::event( QEvent *e )
{
    /* a new window: Qt took the old one down, and the deck back to the DJ */
    if( e->type() == QEvent::WinIdChange && host )
        QMetaObject::invokeMethod( this, "attach", Qt::QueuedConnection );
    return QWidget::event( e );
}

/* where Windows sends the wheel to the focus, not the pointer: on to the deck */
void DjDeck::wheelEvent( QWheelEvent *e )
{
#ifdef _WIN32
    HWND deck = GetWindow( (HWND)winId(), GW_CHILD );
    POINT p;
    if( deck && GetCursorPos( &p ) )
        PostMessageW( deck, WM_MOUSEWHEEL, MAKEWPARAM( 0, (short)e->angleDelta().y() ), MAKELPARAM( p.x, p.y ) );
#endif
    e->accept();
}
