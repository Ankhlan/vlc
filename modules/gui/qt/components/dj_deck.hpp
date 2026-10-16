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
#include <QStringList>

struct dj_host;
struct DjNow;
class DjRecords;
class QLabel;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLineEdit;
class QListWidget;
class QSlider;
class QProgressBar;
class QGridLayout;
class QTimer;

/* The DJ, as a page of the main window, in VLC's own widgets: the records under the playhead,
 * the two decks and the mix between them, what comes next, the moods and a request. */
class DjDeck : public QWidget
{
    Q_OBJECT
public:
    DjDeck( intf_thread_t *, QWidget *parent = NULL );
    virtual ~DjDeck();
    bool isAvailable() const { return host != NULL; }
    QSize sizeHint() const Q_DECL_OVERRIDE { return QSize( 900, 600 ); }

signals:
    void wanted();                  /* the DJ asks for its deck */
    void availabilityChanged( bool );

protected:
    void showEvent( QShowEvent * ) Q_DECL_OVERRIDE;
    void hideEvent( QHideEvent * ) Q_DECL_OVERRIDE;

private slots:
    void attach();
    void tick();
    void airClicked( bool );
    void voiceChosen( int );
    void playNow();
    void queue();
    void mixNext();
    void choose();

private:
    static int HostChanged( vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void * );
    static void Show( void * );
    void command( const QString & );
    void request( const QString &, bool queue );
    void words();

    intf_thread_t         *p_intf;
    const struct dj_host  *host;
    DjNow                 *now;
    QTimer                *timer;
    int                    ticks;

    QLabel       *block, *nextBlock, *said, *mixLine, *transition;
    QCheckBox    *air;
    QComboBox    *voice;
    DjRecords    *records;
    struct Side
    {
        QGroupBox *box;
        QLabel    *title, *artist, *source, *detail, *time;
        QProgressBar *level;
    } side[2];
    QSlider      *xfade;
    QListWidget  *upNext;
    QGroupBox    *moodBox;
    QGridLayout  *moodGrid;
    QStringList   moods, nexts;
    QLineEdit    *field;
};

#endif
