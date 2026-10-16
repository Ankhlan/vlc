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
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QTextStream>
#include <QPainter>
#include <QPainterPath>
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
        setMinimumHeight( 150 );
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
        QFontMetrics fm( font() );
        QRect line( r.x() + 6, r.y() + 3, r.width() - 12, fm.height() );
        QString left, title;
        if( in->state != DJ_EMPTY )
        {
            title = qfu( in->now_playing && *in->now_playing ? in->now_playing : in->t.title ? in->t.title : "" );
            if( in->length_s > 0. ) left = qfu( "\xe2\x88\x92" ) + clock( in->length_s - shown[d] );
        }
        int tw = fm.boundingRect( tag ).width() + 14, lw = left.isEmpty() ? 0 : fm.boundingRect( left ).width() + 14;
        title = fm.elidedText( title, Qt::ElideRight, qMax( 0, line.width() - tw - lw ) );
        QColor veil = pal.color( QPalette::Base );   /* the words over the record, legible */
        veil.setAlpha( 190 );
        p.fillRect( QRect( line.x() - 3, line.y(), tw + fm.boundingRect( title ).width() + 6, line.height() ), veil );
        if( lw ) p.fillRect( QRect( line.right() - lw + 9, line.y(), lw - 6, line.height() ), veil );
        p.setPen( pal.color( QPalette::Text ) );
        p.drawText( line, Qt::AlignLeft | Qt::AlignTop, tag );
        p.drawText( line, Qt::AlignRight | Qt::AlignTop, left );
        QColor dim = pal.color( QPalette::Text );
        dim.setAlpha( 170 );
        p.setPen( dim );
        p.drawText( line.adjusted( tw, 0, 0, 0 ), Qt::AlignLeft | Qt::AlignTop, title );
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

/* The DJ: VLC's cone with its headphones on, nodding to the live record's beat; off air, the
 * headphones come off. */
class DjAvatar : public QWidget
{
public:
    DjAvatar( const DjNow *n, const DjRecords *r, QWidget *parent )
        : QWidget( parent ), now( n ), rec( r ), cone( ":/logo/vlc256.png" )
    {
        setFixedSize( 72, 80 );
    }

protected:
    void paintEvent( QPaintEvent * ) Q_DECL_OVERRIDE
    {
        QPainter p( this );
        p.setRenderHints( QPainter::Antialiasing | QPainter::SmoothPixmapTransform );
        const dj_status_t *st = &now->st;
        bool on = now->have && now->br.on_air;
        int live = now->have ? st->live : -1;
        double pulse = 0., tilt = 0.;
        if( on && live >= 0 && live < DJ_DECKS && st->attached && !st->paused )
        {
            const dj_grid_t *g = &st->deck[live].grid;
            if( g->period > 0. )   /* a nod on every beat, the head the other way on the next */
            {
                double b = (rec->shown[live] * DJ_RATE - g->phase) / g->period, k = floor( b );
                pulse = exp( -(b - k) * 7. );
                tilt = (((long long)k & 1) ? 2.5 : -2.5) * pulse;
            }
            else   /* no steady beat: a sway */
                tilt = 2. * sin( mdate() / (double)CLOCK_FREQ * M_PI );
        }
        double s = (height() - 12) / 256.;
        p.translate( width() / 2., height() - 2 - 4. * pulse );
        p.rotate( tilt );
        p.scale( s, s );
        p.translate( -128., -256. );
        p.setOpacity( on ? 1. : .5 );
        p.drawPixmap( 0, 0, cone );
        if( !on ) return;
        QPainterPath band;   /* over the tip, from cup to cup */
        band.moveTo( 86, 60 );
        band.cubicTo( 84, -26, 172, -26, 170, 60 );
        p.setPen( QPen( QColor( 0x2e, 0x2e, 0x2e ), 11, Qt::SolidLine, Qt::RoundCap ) );
        p.drawPath( band );
        p.setPen( QPen( QColor( 0x6a, 0x6a, 0x6a ), 3, Qt::SolidLine, Qt::RoundCap ) );
        p.drawPath( band );
        QLinearGradient shell( 0, 50, 0, 106 );
        shell.setColorAt( 0., QColor( 0x55, 0x55, 0x55 ) );
        shell.setColorAt( 1., QColor( 0x22, 0x22, 0x22 ) );
        QColor rim = palette().color( QPalette::Highlight );
        for( int side = 0; side < 2; side++ )
        {
            QRectF cup = side ? QRectF( 160, 50, 28, 56 ) : QRectF( 68, 50, 28, 56 );
            p.setPen( QPen( rim, 3 ) );
            p.setBrush( shell );
            p.drawRoundedRect( cup, 11, 11 );
        }
    }

private:
    const DjNow *now;
    const DjRecords *rec;
    QPixmap cone;
};

/* the beat of the bar nearest the playhead, 0 to 3; -1 without a grid */
static int bar_beat( const dj_deck_info_t *in, double pos_s )
{
    const dj_grid_t *g = &in->grid;
    if( g->period <= 0. ) return -1;
    long long k = (long long)floor( (pos_s * DJ_RATE - g->phase) / g->period + .5 );
    return (int)(((k - g->one) % 4 + 4) % 4);
}

/* A deck as a turntable: the record turns at 33 1/3 times its tempo while it plays, the arm
 * walks in with the record, and four lamps count the bar. */
class DjTurntable : public QWidget
{
public:
    DjTurntable( const DjNow *n, const DjRecords *r, int deck, QWidget *parent )
        : QWidget( parent ), now( n ), rec( r ), d( deck ), angle( 0. ), arm( -1. )
    {
        setFixedSize( 100, 116 );
    }

    void step( double dt )
    {
        const dj_status_t *st = &now->st;
        const dj_deck_info_t *in = &st->deck[d];
        bool loaded = now->have && in->state != DJ_EMPTY && in->state != DJ_FAILED;
        if( loaded && st->attached && !st->paused && in->gain > 0.f )
            angle = fmod( angle + dt * in->rate * 200., 360. );   /* 33 1/3 rpm: 200 degrees a second */
        double want = !loaded ? -1. : in->length_s > 0. ? qBound( 0., rec->shown[d] / in->length_s, 1. ) : .3;
        arm += (want - arm) * qMin( 1., dt * 3. );
        update();
    }

protected:
    void paintEvent( QPaintEvent * ) Q_DECL_OVERRIDE
    {
        QPainter p( this );
        p.setRenderHint( QPainter::Antialiasing );
        const QPalette &pal = palette();
        const dj_status_t *st = &now->st;
        const dj_deck_info_t *in = &st->deck[d];
        bool loaded = now->have && in->state != DJ_EMPTY && in->state != DJ_FAILED;
        double side = width(), R = side / 2. - 2.;
        QPointF c( side / 2., side / 2. );
        p.setPen( QPen( pal.color( QPalette::Mid ), 1. ) );
        p.setBrush( pal.color( QPalette::Button ) );
        p.drawEllipse( c, R, R );
        p.save();
        p.translate( c );
        p.rotate( angle );
        p.setPen( Qt::NoPen );
        p.setBrush( pal.color( QPalette::Mid ) );
        for( int i = 0; i < 48; i++ )   /* the platter's strobe */
        {
            p.drawEllipse( QPointF( 0., -(R - 2.5) ), .9, .9 );
            p.rotate( 7.5 );
        }
        if( loaded )
        {
            p.setBrush( QColor( 0x18, 0x18, 0x18 ) );
            p.drawEllipse( QPointF( 0., 0. ), R - 6., R - 6. );
            p.setBrush( Qt::NoBrush );
            p.setPen( QPen( QColor( 0x2c, 0x2c, 0x2c ), .6 ) );
            for( double g = R - 8.; g > R * .38; g -= 2.5 )
                p.drawEllipse( QPointF( 0., 0. ), g, g );
            QColor label = st->live == d ? pal.color( QPalette::Highlight ) : QColor( 0x8c, 0x8c, 0x8c );
            p.setPen( Qt::NoPen );
            p.setBrush( label );
            p.drawEllipse( QPointF( 0., 0. ), R * .33, R * .33 );
            p.setBrush( label.lighter( 135 ) );   /* a stripe on the label, so the turning shows */
            p.drawPie( QRectF( -R * .33, -R * .33, R * .66, R * .66 ), 60 * 16, 40 * 16 );
            QFont f = font();
            f.setBold( true );
            p.setFont( f );
            p.setPen( Qt::white );
            p.drawText( QRectF( -R * .33, -R * .33, R * .66, R * .66 ), Qt::AlignCenter, d ? "B" : "A" );
            p.setBrush( QColor( 0x10, 0x10, 0x10 ) );
            p.setPen( Qt::NoPen );
            p.drawEllipse( QPointF( 0., 0. ), 1.8, 1.8 );
        }
        p.restore();
        if( loaded )   /* the light on the vinyl stays where the lamp is */
        {
            QConicalGradient sheen( c, 45. );
            QColor clear( 255, 255, 255, 0 ), shine( 255, 255, 255, 34 );
            sheen.setColorAt( 0., clear );
            sheen.setColorAt( .1, shine );
            sheen.setColorAt( .2, clear );
            sheen.setColorAt( .5, clear );
            sheen.setColorAt( .6, shine );
            sheen.setColorAt( .7, clear );
            sheen.setColorAt( 1., clear );
            p.setPen( Qt::NoPen );
            p.setBrush( sheen );
            p.drawEllipse( c, R - 6., R - 6. );
        }
        /* the arm: at rest off the record, in the outer groove at the start, by the label at the end */
        double k = side / 128., a = (arm < 0. ? -4. : 19. + 23. * arm) * M_PI / 180.;
        QPointF pivot( side - 12. * k, 14. * k ), dir( -sin( a ), cos( a ) );
        QPointF tip = pivot + dir * (100. * k), back = pivot - dir * (16. * k);
        p.setPen( QPen( QColor( 0x9a, 0x9a, 0x9a ), 3. * k, Qt::SolidLine, Qt::RoundCap ) );
        p.drawLine( back, tip );
        p.setPen( Qt::NoPen );
        p.setBrush( QColor( 0x60, 0x60, 0x60 ) );
        p.drawEllipse( back, 5. * k, 5. * k );
        p.drawEllipse( pivot, 6. * k, 6. * k );
        p.setBrush( QColor( 0x3a, 0x3a, 0x3a ) );
        p.drawEllipse( tip, 3.5 * k, 3.5 * k );
        /* the bar, counted */
        int beat = loaded ? bar_beat( in, rec->shown[d] ) : -1;
        for( int i = 0; i < 4; i++ )
        {
            QPointF at( side / 2. + (i - 1.5) * 16., height() - 8. );
            p.setPen( QPen( pal.color( QPalette::Mid ), 1. ) );
            p.setBrush( i == beat ? (i == 0 ? pal.color( QPalette::Highlight ) : pal.color( QPalette::Highlight ).lighter( 140 ))
                                  : pal.color( QPalette::Base ) );
            p.drawEllipse( at, i == 0 ? 4.5 : 3.5, i == 0 ? 4.5 : 3.5 );
        }
    }

private:
    const DjNow *now;
    const DjRecords *rec;
    int d;
    double angle, arm;
};

/* a channel's level, as VLC's volume shows it, green to red, the peak held a moment */
class DjMeter : public QWidget
{
public:
    DjMeter( QWidget *parent ) : QWidget( parent ), db( -120.f ), peak( -120.f ), peak_at( 0 )
    {
        setFixedWidth( 10 );
        setMinimumHeight( 60 );
    }
    void feed( float v, mtime_t t )
    {
        if( !(v > -120.f) ) v = -120.f;
        db = v;
        if( v >= peak || t - peak_at > 3 * CLOCK_FREQ / 2 )
        {
            peak = v;
            peak_at = t;
        }
        update();
    }

protected:
    void paintEvent( QPaintEvent * ) Q_DECL_OVERRIDE
    {
        QPainter p( this );
        QRectF r = QRectF( rect() ).adjusted( .5, .5, -.5, -.5 );
        p.setPen( palette().color( QPalette::Mid ) );
        p.setBrush( palette().color( QPalette::Base ) );
        p.drawRect( r );
        double f = qBound( 0., (db + 48.) / 48., 1. ), pk = qBound( 0., (peak + 48.) / 48., 1. );
        QLinearGradient g( 0., r.bottom(), 0., r.top() );
        g.setColorAt( 0., QColor( 0x3c, 0xb4, 0x3c ) );
        g.setColorAt( .72, QColor( 0xe0, 0xc4, 0x1e ) );
        g.setColorAt( 1., QColor( 0xd8, 0x34, 0x22 ) );
        QRectF in = r.adjusted( 1.5, 1.5, -1.5, -1.5 );
        p.fillRect( QRectF( in.left(), in.bottom() - in.height() * f, in.width(), in.height() * f ), g );
        if( pk > 0. )
            p.fillRect( QRectF( in.left(), in.bottom() - in.height() * pk, in.width(), 1.5 ), palette().color( QPalette::Text ) );
    }

private:
    float db, peak;
    mtime_t peak_at;
};

/* who runs the requests: the Claude agent dj, its model and effort, from its own definition */
static QString runner()
{
    QFile f( QDir::homePath() + "/.claude/agents/dj.md" );
    QString model, effort;
    if( f.open( QIODevice::ReadOnly | QIODevice::Text ) )
    {
        QTextStream in( &f );
        for( int fences = 0; !in.atEnd() && fences < 2; )
        {
            QString l = in.readLine().trimmed();
            if( l == "---" ) fences++;
            else if( l.startsWith( "model:" ) ) model = l.mid( 6 ).trimmed();
            else if( l.startsWith( "effort:" ) ) effort = l.mid( 7 ).trimmed();
        }
    }
    if( model.isEmpty() ) return QString();
    static const char *const names[][2] = { { "haiku", "Claude Haiku 5.5" }, { "sonnet", "Claude Sonnet 5.5" },
                                            { "opus", "Claude Opus 5.5" }, { "fable", "Claude Fable 5.1" } };
    for( size_t i = 0; i < sizeof names / sizeof *names; i++ )
        if( model == names[i][0] ) model = names[i][1];
    return qtr( "%1, effort %2" ).arg( model, effort.isEmpty() ? qtr( "default" ) : effort );
}

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
    records = new DjRecords( now, this );
    avatar = new DjAvatar( now, records, this );
    said = new QLabel;   /* what the DJ says, or means to do */
    said->setWordWrap( true );
    said->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
    QFont italic = said->font();
    italic.setItalic( true );
    said->setFont( italic );
    QLabel *name = new QLabel( qtr( "DJ" ) ), *card = new QLabel;
    QFont big = bold;
    big.setPointSizeF( big.pointSizeF() * 1.25 );
    name->setFont( big );
    QString run = runner();
    card->setText( run.isEmpty() ? qtr( "the dj module" ) : run );
    quiet( card );
    QString about = qtr( "The DJ in VLC is the dj module: it keeps the programme, picks the records and mixes them." );
    if( !run.isEmpty() ) about += " " + qtr( "Requests from Claude go through the dj agent: %1." ).arg( run );
    avatar->setToolTip( about );
    card->setToolTip( about );
    QHBoxLayout *ident = new QHBoxLayout;
    ident->addWidget( name );
    ident->addSpacing( 8 );
    ident->addWidget( card );
    ident->addStretch();
    QHBoxLayout *when = new QHBoxLayout;
    when->addWidget( block );
    when->addSpacing( 12 );
    when->addWidget( nextBlock );
    when->addStretch();
    QVBoxLayout *who = new QVBoxLayout;
    who->addStretch();
    who->addLayout( ident );
    who->addLayout( when );
    who->addWidget( said );
    who->addStretch();
    head->addWidget( avatar );
    head->addSpacing( 6 );
    head->addLayout( who, 1 );
    head->addWidget( air, 0, Qt::AlignTop );
    head->addSpacing( 6 );
    head->addWidget( voice, 0, Qt::AlignTop );
    main->addLayout( head );
    main->addWidget( records, 3 );

    /* the decks, and the mix between them */
    QHBoxLayout *decks = new QHBoxLayout;
    QGroupBox *mix = new QGroupBox( qtr( "Mix" ) );
    for( int d = 0; d < 2; d++ )
    {
        Side &s = side[d];
        s.box = new QGroupBox( d ? qtr( "Deck B" ) : qtr( "Deck A" ) );
        QHBoxLayout *hv = new QHBoxLayout( s.box );
        s.turntable = new DjTurntable( now, records, d, s.box );
        QVBoxLayout *v = new QVBoxLayout;
        if( d == 0 ) hv->addWidget( s.turntable );
        hv->addLayout( v, 1 );
        if( d == 1 ) hv->addWidget( s.turntable );
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
        s.progress = new QProgressBar;
        s.progress->setRange( 0, 1000 );
        s.progress->setTextVisible( false );
        s.progress->setMaximumHeight( 4 );
        v->addWidget( s.progress );
        s.meter = new DjMeter( this );
    }
    QHBoxLayout *mh = new QHBoxLayout( mix );
    QVBoxLayout *m = new QVBoxLayout;
    transition = new QLabel;
    transition->setAlignment( Qt::AlignCenter );
    transition->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
    xfade = new QSlider( Qt::Horizontal );
    xfade->setRange( -100, 100 );
    xfade->setEnabled( false );   /* the DJ's hand on it, not ours */
    xfade->setToolTip( qtr( "The crossfader: deck A to the left, deck B to the right" ) );
    sync = new QLabel;   /* how close the beats are in a blend */
    sync->setAlignment( Qt::AlignCenter );
    quiet( sync );
    QHBoxLayout *ab = new QHBoxLayout;
    ab->addWidget( new QLabel( "A" ) );
    ab->addStretch();
    ab->addWidget( new QLabel( "B" ) );
    transition->setFont( bold );
    mixProgress = new QProgressBar;
    mixProgress->setRange( 0, 100 );
    mixProgress->setTextVisible( false );
    mixProgress->setMaximumHeight( 4 );
    mixProgress->hide();
    m->addWidget( transition );
    m->addWidget( mixProgress );
    m->addWidget( sync );
    m->addStretch();
    m->addWidget( xfade );
    m->addLayout( ab );
    mh->addWidget( side[0].meter );
    mh->addLayout( m, 1 );
    mh->addWidget( side[1].meter );
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
    play->setIcon( QIcon( ":/toolbar/play_b.svg" ) );
    queued->setIcon( QIcon( ":/buttons/playlist/playlist_add.svg" ) );
    mixNow->setIcon( QIcon( ":/toolbar/next_b.svg" ) );
    pick->setIcon( QIcon( ":/buttons/playlist/shuffle_on.svg" ) );
    spin = new QLabel;   /* the DJ looking for a request */
    spin->setFixedSize( 16, 16 );
    spin->hide();
    QHBoxLayout *ask = new QHBoxLayout;
    ask->addWidget( field );
    ask->addWidget( spin );
    r->addLayout( ask, 0, 0, 1, 2 );
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
    mixLine = new QLabel;
    quiet( mixLine );
    foot->addStretch();
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
    double dt = now->at ? (t - now->at) / (double)CLOCK_FREQ : 0.;
    records->follow( dt );
    now->at = t;
    records->update();
    const dj_status_t *st = &now->st;
    for( int d = 0; d < 2; d++ )
    {
        const dj_deck_info_t *in = &st->deck[d];
        side[d].meter->feed( in->state != DJ_EMPTY && in->gain > 0.f ? in->level_db : -120.f, t );
        side[d].turntable->step( dt );
    }
    xfade->setValue( (int)lroundf( st->xfade * 100.f ) );
    for( int d = 0; d < 2; d++ )
    {
        const dj_deck_info_t *in = &st->deck[d];
        bool known = in->state != DJ_EMPTY && in->length_s > 0.;
        side[d].progress->setVisible( known );
        if( known ) side[d].progress->setValue( (int)(qBound( 0., records->shown[d] / in->length_s, 1. ) * 1000.) );
    }
    mixProgress->setVisible( st->running );
    mixProgress->setValue( (int)(st->progress * 100.) );
    avatar->update();
    if( ticks % 4 == 0 )
    {
        if( now->br.busy )
            spin->setPixmap( QIcon( QString( ":/util/wait%1.svg" ).arg( ticks / 4 % 4 + 1 ) ).pixmap( 16, 16 ) );
        spin->setVisible( now->br.busy );
    }
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
    else if( st->cue >= 0 )
        t = qtr( "Next record cued" );
    else if( st->live >= 0 )
        t = qtr( "Playing it through" );
    transition->setText( t );
    QString sy;
    int a = st->from, b = st->to;
    if( st->running && a >= 0 && b >= 0 && a < DJ_DECKS && b < DJ_DECKS
     && st->deck[a].grid.period > 0. && st->deck[b].grid.period > 0. )
    {
        const dj_deck_info_t *x = &st->deck[a], *y = &st->deck[b];
        double fx = (x->pos_s * DJ_RATE - x->grid.phase) / x->grid.period;
        double fy = (y->pos_s * DJ_RATE - y->grid.phase) / y->grid.period;
        double f = (fx - floor( fx )) - (fy - floor( fy ));
        f -= floor( f + .5 );
        sy = qtr( "Beats matched to %1 ms" ).arg( fabs( f * x->grid.period / DJ_RATE * 1000. / (x->rate > 0. ? x->rate : 1.) ), 0, 'f', 1 );
    }
    else if( st->live >= 0 && st->live < DJ_DECKS && st->deck[st->live].grid.period > 0. )
        sy = QString( "%1 BPM" ).arg( st->deck[st->live].grid.bpm * st->deck[st->live].rate, 0, 'f', 1 );
    sync->setText( sy );
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
    if( error ) said->setText( qfu( br->error ) );
    else if( br->said[0] ) said->setText( qfu( "\xe2\x80\x9c" ) + qfu( br->said ) + qfu( "\xe2\x80\x9d" ) );
    else said->setText( qfu( st->note ) );
    QString line = qtr( "Mixed %1" ).arg( clock( st->mixed_s ) ) + dot + qtr( "48 kHz float" );
    if( st->out_db > -120.f ) line += dot + qtr( "Out %1 dB" ).arg( st->out_db, 0, 'f', 1 );
    if( st->gr_db < -.05f ) line += dot + qtr( "Limiter %1 dB" ).arg( st->gr_db, 0, 'f', 1 );
    mixLine->setText( line );
}
