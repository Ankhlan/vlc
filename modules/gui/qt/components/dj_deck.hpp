/*****************************************************************************
 * dj_deck.hpp : the DJ's deck, docked in the main window
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

#ifndef VLC_QT_DJ_DECK_HPP_
#define VLC_QT_DJ_DECK_HPP_

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "qt.hpp"

#include <QWidget>

struct dj_host;

/* The deck is the dj plugin's own window, drawn on its own thread: this page lends it a place,
 * as the video widget lends one to a video output. */
class DjDeck : public QWidget
{
    Q_OBJECT
public:
    DjDeck( intf_thread_t *, QWidget *parent = NULL );
    virtual ~DjDeck();
    bool isAvailable() const { return host != NULL; }
    QSize sizeHint() const Q_DECL_OVERRIDE;
    QSize minimumSizeHint() const Q_DECL_OVERRIDE;
    QPaintEngine *paintEngine() const Q_DECL_OVERRIDE { return NULL; }

signals:
    void wanted();                  /* the DJ asks for its deck */
    void availabilityChanged( bool );

protected:
    bool event( QEvent * ) Q_DECL_OVERRIDE;
    void wheelEvent( QWheelEvent * ) Q_DECL_OVERRIDE;

private slots:
    void attach();

private:
    static int HostChanged( vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void * );
    static void Show( void * );

    intf_thread_t         *p_intf;
    const struct dj_host  *host;
};

#endif
