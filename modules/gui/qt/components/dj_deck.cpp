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

#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QWheelEvent>
#include <QApplication>
#include <math.h>

/* what the DJ showed last: the deck's thread fills it, a tick at a time */
struct DjNow
{
    dj_status_t st;
    dj_brain_t  br;
    bool        have;
    mtime_t     at, error_at;
    char        error_seen[240];
};

static QString clock( double s )
{
    if( !(s >= 0.) ) s = 0.;
    int t = (int)s, h = t / 3600, m = t / 60 % 60, sec = t % 60;
    if( h ) return QString( "%1:%2:%3" ).arg( h ).arg( m, 2, 10, QChar( '0' ) ).arg( sec, 2, 10, QChar( '0' ) );
    return QString( "%1:%2" ).arg( m ).arg( sec, 2, 10, QChar( '0' ) );
}

/* text half way to the background: what is said about a thing, not the thing */
static void quiet( QWidget *w )
{
    QPalette p = w->palette();
    QColor a = p.color( QPalette::WindowText ), b = p.color( QPalette::Window );
    p.setColor( QPalette::WindowText, QColor( (a.red() * 3 + b.red() * 2) / 5, (a.green() * 3 + b.green() * 2) / 5,
                                              (a.blue() * 3 + b.blue() * 2) / 5 ) );
    w->setPalette( p );
}

/* Both records past one playhead on one time scale, so the beats the mixer matched line up;
 * over each, the whole record and where it is. */
class DjRecords : public QWidget
{
public:
    DjRecords( const DjNow *n, QWidget *parent ) : QWidget( parent ), now( n ), host( NULL ), span( 12. )
    {
        shown[0] = shown[1] = 0.;
        setMinimumHeight( 190 );
        setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
        setToolTip( qtr( "The wheel zooms" ) );
    }
    const DjNow *now;
    const dj_host_t *host;
    double span, shown[2];
    QString url[2];

    /* the mixer moves a deck a block at a time; the screen, every frame */
    void follow( double dt )
    {
        for( int d = 0; d < DJ_DECKS; d++ )
        {
            const dj_deck_info_t *in = &now->st.deck[d];
            QString u = qfu( in->t.url ? in->t.url : "" );
            bool moving = now->st.attached && !now->st.paused && in->gain > 0.f;
            double pred = shown[d] + (moving ? dt * in->rate : 0.);
            if( u != url[d] || fabs( in->pos_s - pred ) > .3 ) pred = in->pos_s;
            else pred += .12 * (in->pos_s - pred);
            shown[d] = pred;
            url[d] = u;
        }
    }

protected:
    void wheelEvent( QWheelEvent *e ) Q_DECL_OVERRIDE
    {
        span = qBound( 3., span * (e->angleDelta().y() > 0 ? .8 : 1.25), 48. );
        update();
        e->accept();
    }

    void paintEvent( QPaintEvent * ) Q_DECL_OVERRIDE
    {
        QPainter p( this );
        const QPalette &pal = palette();
        int w = width(), oh = 16, gap = 4, lh = (height() - 2 * oh - 3 * gap) / 2;
        int y[4] = { 0, oh + gap, oh + 2 * gap + lh, height() - oh };
        p.fillRect( rect(), pal.color( QPalette::Base ) );
        if( !now->have || w < 10 || lh < 10 ) return;
        const dj_status_t *st = &now->st;
        lane( p, 0, QRect( 0, y[1], w, lh ) );
        lane( p, 1, QRect( 0, y[2], w, lh ) );
        whole( p, 0, QRect( 0, y[0], w, oh ) );
        whole( p, 1, QRect( 0, y[3], w, oh ) );
        p.setPen( QPen( pal.color( QPalette::Text ), 1.5 ) );   /* the playhead */
        p.drawLine( QPointF( w / 2., y[1] - 2 ), QPointF( w / 2., y[2] + lh + 2 ) );
        (void)st;
    }

private:
    QColor ink( int d ) const
    {
        const dj_status_t *st = &now->st;
        QColor c = st->live == d ? palette().color( QPalette::Highlight ) : palette().color( QPalette::Text );
        if( st->live != d ) c.setAlpha( 120 );
        return c;
    }

    void columns( QPainter &p, int d, QRect r, double from, double to, int n )
    {
        QVector<float> low( n ), full( n );
        if( !host || !host->wave( d, from, to, n, low.data(), full.data() ) ) return;
        QColor c = ink( d ), soft = c;
        soft.setAlpha( c.alpha() * 2 / 5 );
        QVector<QLineF> a, b;
        double mid = r.center().y() + .5, half = r.height() / 2. - 1, dx = (double)r.width() / n;
        for( int i = 0; i < n; i++ )
        {
            double x = r.x() + (i + .5) * dx;
            if( full[i] >= 0.f ) a << QLineF( x, mid - half * qMin( full[i], 1.f ), x, mid + half * qMin( full[i], 1.f ) );
            if( low[i] >= 0.f ) b << QLineF( x, mid - half * qMin( low[i], 1.f ), x, mid + half * qMin( low[i], 1.f ) );
        }
        p.setPen( QPen( soft, dx ) );
        p.drawLines( a );
        p.setPen( QPen( c, dx ) );
        p.drawLines( b );
    }

    void lane( QPainter &p, int d, QRect r )
    {
        const dj_status_t *st = &now->st;
        const dj_deck_info_t *in = &st->deck[d];
        const QPalette &pal = palette();
        p.setPen( pal.color( QPalette::Mid ) );
        p.drawRect( r.adjusted( 0, 0, -1, -1 ) );
        double t0 = shown[d] - span / 2, t1 = shown[d] + span / 2;
        if( in->state != DJ_EMPTY )
        {
            /* the transition planned, on the record it takes out */
            if( st->from == d && st->span_s > 0. && (st->armed || st->running) )
            {
                double a = (st->at_s - t0) / span * r.width(), b = (st->at_s + st->span_s - t0) / span * r.width();
                QColor h = pal.color( QPalette::Highlight );
                h.setAlpha( 40 );
                p.fillRect( QRectF( r.x() + a, r.y() + 1, b - a, r.height() - 2 ).intersected( QRectF( r ) ), h );
            }
            const dj_grid_t *g = &in->grid;
            if( g->period > 0. )
            {
                long long k0 = (long long)ceil( (t0 * DJ_RATE - g->phase) / g->period );
                long long k1 = (long long)floor( (t1 * DJ_RATE - g->phase) / g->period );
                QColor tick = pal.color( QPalette::Text );
                for( long long k = k0; k <= k1 && k - k0 < 400; k++ )
                {
                    double x = r.x() + ((g->phase + k * g->period) / DJ_RATE - t0) / span * r.width();
                    bool bar = ((k - g->one) % 4 + 4) % 4 == 0;
                    tick.setAlpha( bar ? 110 : 45 );
                    p.setPen( tick );
                    int len = bar ? 7 : 4;
                    p.drawLine( QPointF( x, r.top() + 1 ), QPointF( x, r.top() + len ) );
                    p.drawLine( QPointF( x, r.bottom() - len ), QPointF( x, r.bottom() - 1 ) );
                }
            }
            columns( p, d, r.adjusted( 1, 1, -1, -1 ), t0, t1, qMax( 1, (r.width() - 2) / 2 ) );
        }
        static const char *const states[] = { N_("Empty"), N_("Loading"), N_("Cued"), N_("Live"), N_("Done"), N_("Failed") };
        QString tag = QString( d ? "B" : "A" );
        if( in->state != DJ_LIVE && in->state >= 0 && in->state < 6 )
            tag += "   " + qtr( states[in->state] );
        p.setPen( pal.color( QPalette::Text ) );
        p.drawText( r.adjusted( 6, 3, -6, -3 ), Qt::AlignLeft | Qt::AlignTop, tag );
    }

    void whole( QPainter &p, int d, QRect r )
    {
        const dj_deck_info_t *in = &now->st.deck[d];
        p.fillRect( r, palette().color( QPalette::Window ) );
        if( in->state == DJ_EMPTY || !(in->length_s > 0.) ) return;
        columns( p, d, r, 0., in->length_s, qMax( 1, r.width() / 2 ) );
        double x = r.x() + qBound( 0., shown[d] / in->length_s, 1. ) * r.width();
        p.setPen( QPen( palette().color( QPalette::Text ), 1.5 ) );
        p.drawLine( QPointF( x, r.top() ), QPointF( x, r.bottom() ) );
    }
};

DjDeck::DjDeck( intf_thread_t *_p_intf, QWidget *parent )
    : QWidget( parent ), p_intf( _p_intf ), host( NULL ), now( new DjNow ), ticks( 0 )
{
    memset( now, 0, sizeof *now );
    QVBoxLayout *main = new QVBoxLayout( this );
    main->setContentsMargins( 9, 6, 9, 6 );

    /* the programme's daypart; on air, and the voice */
    QHBoxLayout *head = new QHBoxLayout;
    block = new QLabel;
    QFont bold = block->font();
    bold.setBold( true );
    block->setFont( bold );
    nextBlock = new QLabel;
    quiet( nextBlock );
    air = new QCheckBox( qtr( "On air" ) );
    voice = new QComboBox;
    voice->addItem( qtr( "Voice off" ) );
    voice->addItem( qtr( "Voice between blocks" ) );
    voice->addItem( qtr( "Voice over everything" ) );
    head->addWidget( block );
    head->addSpacing( 12 );
    head->addWidget( nextBlock );
    head->addStretch();
    head->addWidget( air );
    head->addSpacing( 6 );
    head->addWidget( voice );
    main->addLayout( head );

    records = new DjRecords( now, this );
    main->addWidget( records, 3 );

    /* the decks, and the mix between them */
    QHBoxLayout *decks = new QHBoxLayout;
    QGroupBox *mix = new QGroupBox( qtr( "Mix" ) );
    for( int d = 0; d < 2; d++ )
    {
        Side &s = side[d];
        s.box = new QGroupBox( d ? qtr( "Deck B" ) : qtr( "Deck A" ) );
        QVBoxLayout *v = new QVBoxLayout( s.box );
        s.title = new QLabel;
        s.title->setFont( bold );
        s.artist = new QLabel;
        s.source = new QLabel;
        quiet( s.source );
        s.detail = new QLabel;
        quiet( s.detail );
        s.time = new QLabel;
        s.time->setAlignment( Qt::AlignRight | Qt::AlignVCenter );
        QLabel *labels[] = { s.title, s.artist, s.source, s.detail };
        for( QLabel *l : labels )
            l->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
        QHBoxLayout *row = new QHBoxLayout;
        row->addWidget( s.detail, 1 );
        row->addWidget( s.time );
        v->addWidget( s.title );
        v->addWidget( s.artist );
        v->addWidget( s.source );
        v->addStretch();
        v->addLayout( row );
        s.level = new QProgressBar;
        s.level->setRange( 0, 600 );
        s.level->setTextVisible( false );
        s.level->setMaximumHeight( 6 );
    }
    QVBoxLayout *m = new QVBoxLayout( mix );
    transition = new QLabel;
    transition->setAlignment( Qt::AlignCenter );
    transition->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
    xfade = new QSlider( Qt::Horizontal );
    xfade->setRange( -100, 100 );
    xfade->setEnabled( false );   /* the DJ's hand on it, not ours */
    xfade->setToolTip( qtr( "The crossfader: deck A to the left, deck B to the right" ) );
    QHBoxLayout *levels = new QHBoxLayout;
    levels->addWidget( new QLabel( "A" ) );
    levels->addWidget( side[0].level );
    levels->addSpacing( 8 );
    levels->addWidget( side[1].level );
    levels->addWidget( new QLabel( "B" ) );
    m->addWidget( transition );
    m->addStretch();
    m->addWidget( xfade );
    m->addLayout( levels );
    decks->addWidget( side[0].box, 2 );
    decks->addWidget( mix, 1 );
    decks->addWidget( side[1].box, 2 );
    main->addLayout( decks );

    /* what comes next, the moods, a request */
    QHBoxLayout *bottom = new QHBoxLayout;
    QGroupBox *next = new QGroupBox( qtr( "Up next" ) );
    upNext = new QListWidget;
    upNext->setSelectionMode( QAbstractItemView::NoSelection );
    upNext->setFocusPolicy( Qt::NoFocus );
    upNext->setTextElideMode( Qt::ElideRight );
    upNext->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    (new QVBoxLayout( next ))->addWidget( upNext );
    moodBox = new QGroupBox( qtr( "Moods" ) );
    moodGrid = new QGridLayout( moodBox );
    QGroupBox *req = new QGroupBox( qtr( "Request" ) );
    QGridLayout *r = new QGridLayout( req );
    field = new QLineEdit;
    field->setPlaceholderText( qtr( "A mood, an artist, a station, any words" ) );
    field->setClearButtonEnabled( true );
    QPushButton *play = new QPushButton( qtr( "&Play now" ) ), *queued = new QPushButton( qtr( "&Queue" ) );
    QPushButton *mixNow = new QPushButton( qtr( "&Mix next now" ) ), *pick = new QPushButton( qtr( "DJ's &choice" ) );
    mixNow->setToolTip( qtr( "Bring the next record in now" ) );
    pick->setToolTip( qtr( "Let the DJ pick what comes next" ) );
    r->addWidget( field, 0, 0, 1, 2 );
    r->addWidget( play, 1, 0 );
    r->addWidget( queued, 1, 1 );
    r->addWidget( mixNow, 2, 0 );
    r->addWidget( pick, 2, 1 );
    r->setRowStretch( 3, 1 );
    bottom->addWidget( next, 3 );
    bottom->addWidget( moodBox, 4 );
    bottom->addWidget( req, 3 );
    main->addLayout( bottom, 2 );

    QHBoxLayout *foot = new QHBoxLayout;
    said = new QLabel;
    said->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
    mixLine = new QLabel;
    quiet( mixLine );
    foot->addWidget( said, 1 );
    foot->addWidget( mixLine );
    main->addLayout( foot );

    connect( air, &QCheckBox::clicked, this, &DjDeck::airClicked );
    connect( voice, SIGNAL( activated( int ) ), this, SLOT( voiceChosen( int ) ) );
    connect( field, &QLineEdit::returnPressed, this, &DjDeck::playNow );
    connect( play, &QPushButton::clicked, this, &DjDeck::playNow );
    connect( queued, &QPushButton::clicked, this, &DjDeck::queue );
    connect( mixNow, &QPushButton::clicked, this, &DjDeck::mixNext );
    connect( pick, &QPushButton::clicked, this, &DjDeck::choose );

    timer = new QTimer( this );
    timer->setInterval( 33 );
    connect( timer, &QTimer::timeout, this, &DjDeck::tick );

    var_Create( THEPL, DJ_HOST_VAR, VLC_VAR_ADDRESS );
    var_AddCallback( THEPL, DJ_HOST_VAR, HostChanged, this );
    attach();
}

DjDeck::~DjDeck()
{
    var_DelCallback( THEPL, DJ_HOST_VAR, HostChanged, this );
    const dj_host_t *h = (const dj_host_t *)var_GetAddress( THEPL, DJ_HOST_VAR );
    if( h ) h->detach();   /* the DJ shows its own deck again, should anyone ask */
    var_Destroy( THEPL, DJ_HOST_VAR );
    if( now->have && host ) host->status_clean( &now->st );
    delete now;
}

/* any thread: the DJ came or went */
int DjDeck::HostChanged( vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *data )
{
    QMetaObject::invokeMethod( static_cast<DjDeck *>( data ), "attach", Qt::QueuedConnection );
    return VLC_SUCCESS;
}

/* the DJ's thread: someone asked for the deck */
void DjDeck::Show( void *data )
{
    emit static_cast<DjDeck *>( data )->wanted();
}

void DjDeck::attach()
{
    const dj_host_t *h = (const dj_host_t *)var_GetAddress( THEPL, DJ_HOST_VAR );
    if( h && !h->attach( Show, this ) )
        h = NULL;
    if( h == NULL && now->have && host )
    {
        host->status_clean( &now->st );
        now->have = false;
    }
    bool changed = (h != NULL) != (host != NULL);
    host = h;
    records->host = h;
    if( host && isVisible() )
        timer->start();
    if( changed )
        emit availabilityChanged( host != NULL );
}

void DjDeck::showEvent( QShowEvent *e )
{
    QWidget::showEvent( e );
    if( host ) timer->start();
    ticks = 0;
    tick();
}

void DjDeck::hideEvent( QHideEvent *e )
{
    timer->stop();
    QWidget::hideEvent( e );
}

void DjDeck::tick()
{
    if( !host ) return;
    if( now->have ) host->status_clean( &now->st );
    now->have = host->status( &now->st, &now->br );
    if( !now->have ) return;
    mtime_t t = mdate();
    records->follow( now->at ? (t - now->at) / (double)CLOCK_FREQ : 0. );
    now->at = t;
    records->update();
    const dj_status_t *st = &now->st;
    for( int d = 0; d < 2; d++ )
    {
        const dj_deck_info_t *in = &st->deck[d];
        bool heard = in->state != DJ_EMPTY && in->gain > 0.f && in->level_db > -60.f;
        side[d].level->setValue( heard ? (int)((in->level_db + 60.f) * 10.f) : 0 );
    }
    xfade->setValue( (int)lroundf( st->xfade * 100.f ) );
    if( ticks++ % 8 == 0 ) words();
}

static void elide( QLabel *l, const QString &s )
{
    l->setText( l->fontMetrics().elidedText( s, Qt::ElideRight, qMax( 40, l->width() ) ) );
    l->setToolTip( s.length() > 40 ? s : QString() );
}

void DjDeck::command( const QString &verb )
{
    if( host ) host->command( qtu( verb ) );
}

void DjDeck::request( const QString &text, bool queue )
{
    QString t = text.trimmed();
    if( t.isEmpty() ) return;
    command( "request?text=" + QString::fromLatin1( QUrl::toPercentEncoding( t ) ) + (queue ? "&queue=1" : "") );
    if( text == field->text() ) field->clear();
}

void DjDeck::airClicked( bool on )   { command( on ? "air?on=1" : "air?on=0" ); }
void DjDeck::voiceChosen( int i )    { command( i == 1 ? "air?voice=blocks" : i == 2 ? "air?voice=all" : "air?voice=off" ); }
void DjDeck::playNow()               { request( field->text(), QApplication::keyboardModifiers() & Qt::ShiftModifier ); }
void DjDeck::queue()                 { request( field->text(), true ); }
void DjDeck::mixNext()               { command( "next" ); }
void DjDeck::choose()                { command( "choose" ); }

/* the words, a few times a second */
void DjDeck::words()
{
    static const char *const states[] = { N_("Empty"), N_("Loading"), N_("Cued"), N_("Live"), N_("Done"), N_("Failed") };
    static const char *const kinds[] = { "", N_("Start"), N_("Blend"), N_("Fade"), N_("Echo out") };
    const dj_status_t *st = &now->st;
    const dj_brain_t *br = &now->br;
    const QString dot = qfu( " \xc2\xb7 " );

    block->setText( qfu( br->block ) );
    nextBlock->setText( br->next_block[0] ? qtr( "Next: %1 at %2" ).arg( qfu( br->next_block ), qfu( br->next_at ) ) : QString() );
    air->setChecked( br->on_air );
    if( !voice->view()->isVisible() )
        voice->setCurrentIndex( qBound( 0, br->voice, 2 ) );

    for( int d = 0; d < 2; d++ )
    {
        const dj_deck_info_t *in = &st->deck[d];
        Side &s = side[d];
        bool any = in->state != DJ_EMPTY;
        QString title = qfu( in->now_playing && *in->now_playing ? in->now_playing : in->t.title ? in->t.title : "" );
        QString name = d ? qtr( "Deck B" ) : qtr( "Deck A" );
        s.box->setTitle( st->live == d ? name + dot + qtr( "Live" ) : name );
        elide( s.title, any ? title : qtr( "Waiting for the next record" ) );
        elide( s.artist, any && in->t.artist ? qfu( in->t.artist ) : QString() );
        elide( s.source, any && in->t.label ? qfu( in->t.label ) : QString() );
        QStringList parts;
        if( any && in->state != DJ_LIVE && in->state < 6 ) parts << qtr( states[in->state] );
        if( in->grid.period > 0. ) parts << QString( "%1 BPM" ).arg( in->grid.bpm * in->rate, 0, 'f', 1 );
        else if( in->state == DJ_LIVE || in->state == DJ_CUED ) parts << qtr( "No steady beat" );
        if( any && in->codec[0] )
            parts << (in->lossless ? QString( "%1 %2-bit %3 kHz" ).arg( qfu( in->codec ) ).arg( in->src_bits ).arg( in->src_rate / 1000. )
                     : in->kbps > 0 ? QString( "%1 %2 kbps" ).arg( qfu( in->codec ) ).arg( in->kbps ) : qfu( in->codec ));
        elide( s.detail, parts.join( dot ) );
        s.time->setText( !any ? QString() : in->length_s > 0. ? clock( in->pos_s ) + " / " + clock( in->length_s ) : clock( in->pos_s ) );
    }

    QString kind = st->kind > 0 && st->kind < 5 ? qtr( kinds[st->kind] ) : QString(), t;
    if( st->running && !kind.isEmpty() )
        t = qtr( "%1, %2 %" ).arg( kind ).arg( (int)(st->progress * 100.) );
    else if( st->armed && !kind.isEmpty() )
        t = st->beats > 0 ? qtr( "%1 over %2 beats in %3" ).arg( kind ).arg( st->beats ).arg( clock( st->starts_in_s ) )
                          : qtr( "%1 in %2" ).arg( kind, clock( st->starts_in_s ) );
    else if( st->loading )
        t = qtr( "Cueing the next record" );
    transition->setText( t );
    transition->setToolTip( qfu( st->note ) );

    QStringList l;
    for( int i = 0; i < br->n_next && i < 6; i++ ) l << qfu( br->next[i] );
    if( l != nexts )
    {
        nexts = l;
        upNext->clear();
        upNext->addItems( l );
    }
    l.clear();
    for( int i = 0; i < br->n_moods && i < 24; i++ ) l << qfu( br->moods[i] );
    if( l != moods )
    {
        moods = l;
        QLayoutItem *it;
        while( (it = moodGrid->takeAt( 0 )) != NULL )
        {
            delete it->widget();
            delete it;
        }
        for( int i = 0; i < l.size(); i++ )
        {
            QPushButton *b = new QPushButton( l[i] );
            QString m = l[i];
            connect( b, &QPushButton::clicked, this, [this, m]() { request( m, false ); } );
            moodGrid->addWidget( b, i / 3, i % 3 );
        }
    }

    if( strcmp( br->error, now->error_seen ) )
    {
        snprintf( now->error_seen, sizeof now->error_seen, "%s", br->error );
        now->error_at = mdate();
    }
    bool error = br->error[0] && mdate() - now->error_at < 30 * CLOCK_FREQ;
    QPalette pal = palette();
    if( error ) pal.setColor( QPalette::WindowText, QColor( 0xc4, 0x2b, 0x1c ) );
    said->setPalette( pal );
    elide( said, qfu( error ? br->error : br->said[0] ? br->said : st->note ) );
    QString line = qtr( "Mixed %1" ).arg( clock( st->mixed_s ) ) + dot + qtr( "48 kHz float" );
    if( st->out_db > -120.f ) line += dot + qtr( "Out %1 dB" ).arg( st->out_db, 0, 'f', 1 );
    if( st->gr_db < -.05f ) line += dot + qtr( "Limiter %1 dB" ).arg( st->gr_db, 0, 'f', 1 );
    mixLine->setText( line );
}
