/*****************************************************************************
 * dj_mix.c : the DJ's two decks and its mixer
 *****************************************************************************
 * Copyright (C) 2026 Ankhlan Erdenebat
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*
 * A deck is a private input whose stream output decodes the track to 48 kHz float stereo,
 * #transcode{acodec=f32l}:djdeck, and writes it into the deck's ring as fast as the ring
 * takes it. Nothing in that chain keeps time, so a deck reads eighty seconds ahead of what
 * is heard: the beat grid, the loudness, where the music ends, all come from audio that has
 * not played yet.
 *
 * The mix is a VLC item, djmix://, an access_demux the playlist plays like any stream. It
 * pulls 1024 frames at a time from the mixer: each deck through its tempo, bass EQ, echo and
 * fader, then the master and a limiter that only acts above -0.5 dBFS. A deck at its own
 * speed is copied sample for sample; a deck matched to another's tempo goes through a
 * 96-tap Kaiser-windowed sinc.
 *
 * The engine's thread loads and unloads decks, analyses what they read and plans the next
 * transition; the mixer plays the plan to the frame:
 *   blend  beatmatched, the incoming bass cut until the swap halfway through;
 *   echo   the outgoing deck into a dotted-eighth echo, cut on a downbeat, the next one in;
 *   fade   equal power, for what has no beat to match;
 *   start  out of silence.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_demux.h>
#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_es.h>

#include <math.h>

#include "dj.h"

#define RING_S     80          /* read ahead */
#define KEEP_S     4           /* kept behind the read position */
#define HIST_S     30          /* analysis kept behind it */
#define BLOCK      1024        /* frames the output asks for at a time */
#define SUB        64          /* automation steps */
#define HALF       48
#define TAPS       (2 * HALF)
#define PHASES     256
#define ECHO_MAX   (2 * DJ_RATE)
#define LIM_LA     96          /* limiter look-ahead, 2 ms */
#define LIM_CEIL   0.944f      /* -0.5 dBFS */
#define TARGET     (-14.f)     /* LUFS every track is trimmed toward */
#define KILL       (-26.f)     /* the bass EQ, all the way down */
#define SLEW       0.0002      /* tempo ride back to the record's own: 0.02 % a second */
#define LATENCY_S  0.35        /* mixed to heard: the output's pts delay and the device */
#define OV_HOPS    100         /* the overview: the RMS of every tenth of a second */
#define OV_MAX     (12 * 36000) /* of them: twelve hours */

enum { EV_BEGAN = 1, EV_DONE = 2 };

/*****************************************************************************
 * Filters: RBJ biquads, direct form I in double
 *****************************************************************************/
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } bq_t;

static void bq_set( bq_t *f, double b0, double b1, double b2, double a0, double a1, double a2 )
{
    f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0; f->a1 = a1 / a0; f->a2 = a2 / a0;
}

static void bq_clear( bq_t *f ) { f->x1 = f->x2 = f->y1 = f->y2 = 0.; }

static void bq_lowpass( bq_t *f, double hz, double q )
{
    double w = 2. * M_PI * hz / DJ_RATE, c = cos( w ), al = sin( w ) / (2. * q);
    bq_set( f, (1. - c) / 2., 1. - c, (1. - c) / 2., 1. + al, -2. * c, 1. - al );
}

static void bq_highpass( bq_t *f, double hz, double q )
{
    double w = 2. * M_PI * hz / DJ_RATE, c = cos( w ), al = sin( w ) / (2. * q);
    bq_set( f, (1. + c) / 2., -(1. + c), (1. + c) / 2., 1. + al, -2. * c, 1. - al );
}

/* shelf slope 1; at 0 dB the numerator is the denominator: the identity */
static void bq_lowshelf( bq_t *f, double hz, double db )
{
    double A = pow( 10., db / 40. ), w = 2. * M_PI * hz / DJ_RATE, c = cos( w );
    double sa = sqrt( A ) * sin( w ) * M_SQRT2;
    bq_set( f, A * ((A + 1.) - (A - 1.) * c + sa), 2. * A * ((A - 1.) - (A + 1.) * c),
            A * ((A + 1.) - (A - 1.) * c - sa), (A + 1.) + (A - 1.) * c + sa,
            -2. * ((A - 1.) + (A + 1.) * c), (A + 1.) + (A - 1.) * c - sa );
}

static inline double bq_run( bq_t *f, double x )
{
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y;
    return y;
}

/* BS.1770 K-weighting at 48 kHz: the head's shelf, then the high-pass */
static void bq_kweight( bq_t *shelf, bq_t *hp )
{
    shelf->b0 = 1.53512485958697; shelf->b1 = -2.69169618940638; shelf->b2 = 1.19839281085285;
    shelf->a1 = -1.69065929318241; shelf->a2 = 0.73248077421585;
    hp->b0 = 1.; hp->b1 = -2.; hp->b2 = 1.;
    hp->a1 = -1.99004745483398; hp->a2 = 0.99007225036621;
    bq_clear( shelf ); bq_clear( hp );
}

/*****************************************************************************
 * Types
 *****************************************************************************/
typedef struct
{
    dj_mix_t       *mix;
    int             id;
    int             state;                 /* m->lock */
    dj_track_t      t;
    input_item_t   *item;
    input_thread_t *input;

    /* the ring, between the deck's decoder thread (the writer) and the mixer */
    vlc_mutex_t     lock;
    vlc_cond_t      cond;
    float          *buf;                   /* stereo frames */
    int64_t         cap, written, floor;   /* the writer may overwrite frames before floor */
    float          *env_low, *env_k, *env_peak;   /* a value a hop: kick band, K-weighted, peak */
    int64_t         hop_cap, hops;
    float          *ov;                    /* the whole record: kick band, K-weighted pairs */
    int64_t         ov_cap, ov_n;
    bool            dead, failed, stopping;
    /* the writer's own */
    bq_t            lp[2], kw[4];
    double          acc_low, acc_k;
    float           acc_peak;
    int             acc_n;
    int64_t         hops_w, ov_w;
    float           ov_l, ov_k;
    int             ov_acc;

    /* the mixer's, m->lock */
    double          pos, rate, rate_target, snap_rate;
    int             snap_left;
    bool            resample, audible, underrun, shelf_on, echo_on;
    int64_t         start_at;              /* the mix frame it sounds from */
    float           gain, trim, trim_target, low_db, level;
    bq_t            shelf[2];
    float           shelf_db;
    float          *echo;
    int             echo_len, echo_w;
    float           echo_send, dry;
    bq_t            ehp[2], elp[2];
    mtime_t         starved_since;

    /* what the analysis found, m->lock */
    dj_grid_t       grid;
    double          cue, first_audible;
    float           loud, peak_db;
    mtime_t         loaded_at, regrid_at;
} deck_t;

typedef struct
{
    int     kind, from, to, beats, urgency;
    double  at;          /* the outgoing deck's position where it begins */
    double  to_cue, to_rate;
    double  beat;        /* mix frames a beat */
    double  length;      /* mix frames, fades and starts */
    bool    armed, running;
    int64_t t0;
} trans_t;

struct dj_mix
{
    vlc_object_t *owner;
    unsigned      refs;                    /* g_lock */
    vlc_thread_t  thread;
    bool          started, quit, clear;
    vlc_mutex_t   lock;
    vlc_cond_t    wake;
    deck_t        deck[DJ_DECKS];
    int           live, cue, want;
    dj_track_t    pending;
    bool          has_pending;
    trans_t       tr;
    int64_t       time;                    /* frames mixed */
    float         master, master_target;
    float         lim_gain, lim_buf[2 * LIM_LA], lim_need[LIM_LA];
    int           lim_w, lim_hot;
    float         out_level;
    demux_t      *out;
    bool          paused;
    unsigned      events;
    char          note[200];
    float        *sinc, *tmp;
    dj_log_cb     log;
    void         *opaque;
};

static vlc_mutex_t g_lock = VLC_STATIC_MUTEX;
static dj_mix_t   *g_mix;   /* the engine the output plays */

void dj_track_Copy( dj_track_t *dst, const dj_track_t *src )
{
    dst->url = src->url ? strdup( src->url ) : NULL;
    dst->title = src->title ? strdup( src->title ) : NULL;
    dst->artist = src->artist ? strdup( src->artist ) : NULL;
    dst->label = src->label ? strdup( src->label ) : NULL;
    dst->length_ms = src->length_ms;
    dst->endless = src->endless;
    dst->requested = src->requested;
    dst->block = src->block;
    dst->pick = src->pick;
}

void dj_track_Clean( dj_track_t *t )
{
    free( t->url ); free( t->title ); free( t->artist ); free( t->label );
    memset( t, 0, sizeof *t );
}

static void say( dj_mix_t *m, const char *verb, const char *fmt, ... )
{
    if( m->log == NULL ) return;
    char *what = NULL;
    va_list ap;
    va_start( ap, fmt );
    if( vasprintf( &what, fmt, ap ) < 0 ) what = NULL;
    va_end( ap );
    m->log( m->opaque, verb, what ? what : "" );
    free( what );
}

/*****************************************************************************
 * The deck's writer: its own decoder thread, through the djdeck stream output
 *****************************************************************************/
/* grown under the lock its readers take; written past what they may read without it */
static void ov_push( deck_t *d )
{
    if( d->ov_w == d->ov_cap && d->ov_cap < OV_MAX )
    {
        int64_t cap = d->ov_cap ? 2 * d->ov_cap : 4096;
        if( cap > OV_MAX ) cap = OV_MAX;
        vlc_mutex_lock( &d->lock );
        float *ov = realloc( d->ov, cap * 2 * sizeof (float) );
        if( ov ) { d->ov = ov; d->ov_cap = cap; }
        vlc_mutex_unlock( &d->lock );
    }
    if( d->ov_w < d->ov_cap )
    {
        d->ov[2 * d->ov_w] = sqrtf( d->ov_l / OV_HOPS );
        d->ov[2 * d->ov_w + 1] = sqrtf( d->ov_k / OV_HOPS );
        d->ov_w++;
    }
    d->ov_l = d->ov_k = 0.f;
    d->ov_acc = 0;
}

static void deck_analyse( deck_t *d, const float *p, size_t n )
{
    for( size_t i = 0; i < n; i++ )
    {
        double l = p[2 * i], r = p[2 * i + 1];
        double y = bq_run( &d->lp[1], bq_run( &d->lp[0], 0.5 * (l + r) ) );
        double kl = bq_run( &d->kw[1], bq_run( &d->kw[0], l ) );
        double kr = bq_run( &d->kw[3], bq_run( &d->kw[2], r ) );
        d->acc_low += y * y;
        d->acc_k += kl * kl + kr * kr;
        float a = fabsf( p[2 * i] ), b = fabsf( p[2 * i + 1] );
        if( a > d->acc_peak ) d->acc_peak = a;
        if( b > d->acc_peak ) d->acc_peak = b;
        if( ++d->acc_n == DJ_HOP )
        {
            int64_t h = d->hops_w % d->hop_cap;
            d->env_low[h] = (float)(d->acc_low / DJ_HOP);
            d->env_k[h] = (float)(d->acc_k / DJ_HOP);
            d->env_peak[h] = d->acc_peak;
            d->ov_l += d->env_low[h];
            d->ov_k += d->env_k[h];
            if( ++d->ov_acc == OV_HOPS ) ov_push( d );
            d->hops_w++;
            d->acc_low = d->acc_k = 0.;
            d->acc_peak = 0.f;
            d->acc_n = 0;
        }
    }
}

/* waits while the ring is full: the mixer's pace is the deck's */
static void deck_write( deck_t *d, const float *src, size_t frames )
{
    while( frames > 0 )
    {
        vlc_mutex_lock( &d->lock );
        int64_t space = 0;
        while( !d->stopping && (space = d->cap - (d->written - d->floor)) <= 0 )
            vlc_cond_timedwait( &d->cond, &d->lock, mdate() + CLOCK_FREQ / 5 );
        if( d->stopping || d->buf == NULL )
        {
            vlc_mutex_unlock( &d->lock );
            return;
        }
        int64_t w = d->written;
        vlc_mutex_unlock( &d->lock );

        size_t n = frames < (size_t)space ? frames : (size_t)space;
        if( n > 8192 ) n = 8192;
        for( size_t i = 0; i < n; )
        {
            int64_t at = (w + (int64_t)i) % d->cap;
            size_t run = n - i;
            if( (int64_t)run > d->cap - at ) run = (size_t)(d->cap - at);
            memcpy( d->buf + 2 * at, src + 2 * i, run * 2 * sizeof *src );
            i += run;
        }
        deck_analyse( d, src, n );

        vlc_mutex_lock( &d->lock );
        d->written = w + (int64_t)n;
        d->hops = d->hops_w;
        d->ov_n = d->ov_w;
        vlc_cond_broadcast( &d->cond );
        vlc_mutex_unlock( &d->lock );
        src += 2 * n;
        frames -= n;
    }
}

struct sout_stream_sys_t
{
    deck_t *deck;
};

static sout_stream_id_sys_t *DeckAdd( sout_stream_t *s, const es_format_t *fmt )
{
    if( fmt->i_cat != AUDIO_ES ) return NULL;
    if( fmt->i_codec != VLC_CODEC_FL32 || fmt->audio.i_channels != 2 || fmt->audio.i_rate != DJ_RATE )
    {
        msg_Err( s, "a deck takes float stereo at %d Hz, not %4.4s, %u channels, %u Hz", DJ_RATE,
                 (const char *)&fmt->i_codec, fmt->audio.i_channels, fmt->audio.i_rate );
        return NULL;
    }
    return (sout_stream_id_sys_t *)s->p_sys;
}

static void DeckDel( sout_stream_t *s, sout_stream_id_sys_t *id )
{
    VLC_UNUSED(s); VLC_UNUSED(id);
}

static int DeckSend( sout_stream_t *s, sout_stream_id_sys_t *id, block_t *b )
{
    VLC_UNUSED(id);
    deck_t *d = s->p_sys->deck;
    while( b )
    {
        block_t *next = b->p_next;
        deck_write( d, (const float *)b->p_buffer, b->i_buffer / (2 * sizeof (float)) );
        block_Release( b );
        b = next;
    }
    return VLC_SUCCESS;
}

int DjDeckOpen( vlc_object_t *obj )
{
    sout_stream_t *s = (sout_stream_t *)obj;
    deck_t *d = var_InheritAddress( obj, "dj-deck" );
    if( d == NULL )
    {
        msg_Err( s, "djdeck belongs to the DJ: there is no deck here" );
        return VLC_EGENERIC;
    }
    sout_stream_sys_t *sys = malloc( sizeof *sys );
    if( sys == NULL ) return VLC_ENOMEM;
    sys->deck = d;
    s->p_sys = sys;
    s->pf_add = DeckAdd;
    s->pf_del = DeckDel;
    s->pf_send = DeckSend;
    return VLC_SUCCESS;
}

void DjDeckClose( vlc_object_t *obj )
{
    free( ((sout_stream_t *)obj)->p_sys );
}

/*****************************************************************************
 * Loading and unloading, on the engine's thread
 *****************************************************************************/
static int DeckEvent( vlc_object_t *obj, const char *var, vlc_value_t old, vlc_value_t cur, void *data )
{
    VLC_UNUSED(var); VLC_UNUSED(old);
    deck_t *d = data;
    if( cur.i_int != INPUT_EVENT_DEAD && cur.i_int != INPUT_EVENT_STATE ) return VLC_SUCCESS;
    bool error = var_GetInteger( obj, "state" ) == ERROR_S;
    if( cur.i_int == INPUT_EVENT_DEAD || error )
    {
        vlc_mutex_lock( &d->lock );
        if( cur.i_int == INPUT_EVENT_DEAD ) d->dead = true;
        if( error ) d->failed = true;
        vlc_cond_broadcast( &d->cond );
        vlc_mutex_unlock( &d->lock );
        vlc_cond_signal( &d->mix->wake );
    }
    return VLC_SUCCESS;
}

static void deck_free_buffers( deck_t *d )
{
    vlc_mutex_lock( &d->lock );
    float *b = d->buf, *l = d->env_low, *k = d->env_k, *p = d->env_peak, *o = d->ov;
    d->buf = d->env_low = d->env_k = d->env_peak = d->ov = NULL;
    d->ov_cap = d->ov_n = 0;
    vlc_mutex_unlock( &d->lock );
    free( b ); free( l ); free( k ); free( p ); free( o );
    free( d->echo );
    d->echo = NULL;
}

/* the deck's thread is joined before its buffers go */
static void deck_unload( deck_t *d )
{
    vlc_mutex_lock( &d->lock );
    d->stopping = true;
    vlc_cond_broadcast( &d->cond );
    vlc_mutex_unlock( &d->lock );
    if( d->input )
    {
        var_DelCallback( d->input, "intf-event", DeckEvent, d );
        input_Stop( d->input );
        input_Close( d->input );
        d->input = NULL;
    }
    if( d->item )
    {
        input_item_Release( d->item );
        d->item = NULL;
    }
    deck_free_buffers( d );
    dj_track_Clean( &d->t );
    vlc_mutex_lock( &d->lock );
    d->stopping = false;
    vlc_mutex_unlock( &d->lock );
}

/* takes the track */
static bool deck_load( dj_mix_t *m, deck_t *d, dj_track_t *t )
{
    int64_t cap = (int64_t)RING_S * DJ_RATE, hop_cap = (int64_t)(RING_S + HIST_S) * DJ_RATE / DJ_HOP;
    float *buf = malloc( cap * 2 * sizeof (float) );
    float *el = malloc( hop_cap * sizeof (float) ), *ek = malloc( hop_cap * sizeof (float) );
    float *ep = malloc( hop_cap * sizeof (float) );
    d->echo = calloc( 2 * ECHO_MAX, sizeof (float) );
    if( buf == NULL || el == NULL || ek == NULL || ep == NULL || d->echo == NULL )
    {
        free( buf ); free( el ); free( ek ); free( ep );
        free( d->echo );
        d->echo = NULL;
        dj_track_Clean( t );
        return false;
    }
    vlc_mutex_lock( &d->lock );
    d->buf = buf; d->env_low = el; d->env_k = ek; d->env_peak = ep;
    d->cap = cap; d->hop_cap = hop_cap;
    d->written = d->floor = d->hops = 0;
    d->dead = d->failed = d->stopping = false;
    vlc_mutex_unlock( &d->lock );
    d->hops_w = d->ov_w = 0;
    d->regrid_at = 0;
    d->ov_l = d->ov_k = 0.f;
    d->ov_acc = 0;
    bq_lowpass( &d->lp[0], 150., M_SQRT1_2 );
    bq_clear( &d->lp[0] );
    d->lp[1] = d->lp[0];
    bq_kweight( &d->kw[0], &d->kw[1] );
    bq_kweight( &d->kw[2], &d->kw[3] );
    d->acc_low = d->acc_k = 0.;
    d->acc_peak = 0.f;
    d->acc_n = 0;

    d->pos = 0.; d->rate = d->rate_target = 1.; d->snap_left = 0;
    d->resample = d->audible = d->underrun = d->shelf_on = d->echo_on = false;
    d->start_at = 0;
    d->gain = 0.f; d->trim = d->trim_target = 1.f; d->low_db = 0.f; d->level = 0.f; d->shelf_db = 0.f;
    d->echo_w = 0; d->echo_len = 1; d->echo_send = 0.f; d->dry = 1.f;
    d->starved_since = 0;
    memset( &d->grid, 0, sizeof d->grid );
    d->cue = d->first_audible = 0.;
    d->loud = TARGET; d->peak_db = 0.f;
    d->loaded_at = mdate();
    d->t = *t;
    memset( t, 0, sizeof *t );

    input_item_t *it = input_item_NewExt( d->t.url, d->t.title ? d->t.title : d->t.url,
                                          d->t.length_ms > 0 ? d->t.length_ms * 1000 : -1,
                                          ITEM_TYPE_UNKNOWN, ITEM_NET_UNKNOWN );
    if( it == NULL ) goto error;
    /* decoded, and resampled to the mix's rate by the samplerate module, as fast as the ring takes it */
    input_item_AddOption( it, ":sout=#transcode{acodec=f32l,samplerate=48000,channels=2}:djdeck",
                          VLC_INPUT_OPTION_TRUSTED );
    input_item_AddOption( it, ":no-sout-video", VLC_INPUT_OPTION_TRUSTED );
    input_item_AddOption( it, ":no-sout-spu", VLC_INPUT_OPTION_TRUSTED );
    if( strncmp( d->t.url, "file:", 5 ) )
    {
        input_item_AddOption( it, ":network-caching=3000", VLC_INPUT_OPTION_TRUSTED );
        if( !strncmp( d->t.url, "http://", 7 ) )
            input_item_AddOption( it, ":http-reconnect", VLC_INPUT_OPTION_TRUSTED );
    }
    input_thread_t *in = input_Create( m->owner, it, "dj deck", NULL, NULL );
    if( in == NULL )
    {
        input_item_Release( it );
        goto error;
    }
    var_Create( in, "dj-deck", VLC_VAR_ADDRESS );
    var_SetAddress( in, "dj-deck", d );
    var_AddCallback( in, "intf-event", DeckEvent, d );
    if( input_Start( in ) )
    {
        var_DelCallback( in, "intf-event", DeckEvent, d );
        input_Close( in );
        input_item_Release( it );
        goto error;
    }
    d->item = it;
    d->input = in;
    return true;
error:
    deck_free_buffers( d );
    dj_track_Clean( &d->t );
    return false;
}

/*****************************************************************************
 * Listening ahead: beat grid, loudness, the end of the music
 *****************************************************************************/
/* hops [*h0, *h1) of a deck, clipped to what it holds, copied out of its rings */
static bool hops_get( deck_t *d, int64_t *h0, int64_t *h1, float **low, float **kw, float **pk )
{
    *low = *kw = *pk = NULL;
    vlc_mutex_lock( &d->lock );
    int64_t lo = d->hops - d->hop_cap + 1;
    if( lo < 0 ) lo = 0;
    if( *h0 < lo ) *h0 = lo;
    if( *h1 > d->hops ) *h1 = d->hops;
    int64_t n = *h1 - *h0;
    bool ok = n > 0 && d->env_low != NULL;
    if( ok )
    {
        *low = malloc( n * sizeof (float) );
        *kw = malloc( n * sizeof (float) );
        *pk = malloc( n * sizeof (float) );
        ok = *low && *kw && *pk;
        for( int64_t i = 0; ok && i < n; i++ )
        {
            int64_t h = (*h0 + i) % d->hop_cap;
            (*low)[i] = d->env_low[h];
            (*kw)[i] = d->env_k[h];
            (*pk)[i] = d->env_peak[h];
        }
    }
    vlc_mutex_unlock( &d->lock );
    if( !ok ) { free( *low ); free( *kw ); free( *pk ); *low = *kw = *pk = NULL; }
    return ok;
}

/* the rise of the log energy, smoothed over 8 ms, across 8 ms */
static void onset( const float *e, int64_t n, float *s, float *o )
{
    double acc = 0.;
    for( int64_t i = 0; i < n; i++ )
    {
        acc += e[i];
        if( i >= 8 ) acc -= e[i - 8];
        s[i] = logf( (float)(acc > 0. ? acc / 8. : 0.) + 1e-10f );
    }
    for( int64_t i = 0; i < n; i++ )
    {
        float d = i >= 16 ? s[i] - s[i - 8] : 0.f;
        o[i] = d > 0.f ? d : 0.f;
    }
}

static double tempo_score( const double *acf, int l )
{
    double sc = acf[l] + 0.5 * acf[2 * l] + 0.25 * acf[4 * l];
    double bpm = 15000. / l, oct = log2( bpm / 122. ) / 0.7;
    return sc > 0. ? sc * exp( -0.5 * oct * oct ) : sc;
}

static int cmp_double( const void *a, const void *b )
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* least squares through the kicks nearest a grid, the strays trimmed: one pass of the fit.
 * Answers the kicks kept; the grid's period and phase move, jitter and the period's
 * standard error come out */
static int beat_pass( const float *o, int64_t n, int64_t lim, double thr, double win,
                      double *P, double *phi, double *jit, double *sd, int *beats,
                      double *bk, double *bt, double *bw, double *res )
{
    int hits = 0;
    *beats = 0;
    for( int k = 0; *beats < 2048; k++ )
    {
        double c = *phi + k * *P;
        if( c >= lim - 2 ) break;
        if( c < 1. ) continue;
        (*beats)++;
        int64_t a = (int64_t)ceil( c - win ), b = (int64_t)floor( c + win );
        if( a < 1 ) a = 1;
        if( b > n - 2 ) b = n - 2;
        int64_t bi = -1;
        float bv = 0.f;
        for( int64_t i = a; i <= b; i++ ) if( o[i] > bv ) { bv = o[i]; bi = i; }
        if( bi < 0 || bv < thr || bi == a || bi == b ) continue;
        double q0 = o[bi - 1], q1 = o[bi], q2 = o[bi + 1], dq = q0 - 2. * q1 + q2;
        bk[hits] = k;
        bt[hits] = bi + (dq < 0. ? 0.5 * (q0 - q2) / dq : 0.);
        bw[hits] = bv;
        hits++;
    }
    for( int it = 0; it < 5; it++ )
    {
        if( hits < 12 ) return 0;
        double S = 0., Sk = 0., Skk = 0., St = 0., Skt = 0.;
        for( int i = 0; i < hits; i++ )
        {
            S += bw[i]; Sk += bw[i] * bk[i]; Skk += bw[i] * bk[i] * bk[i];
            St += bw[i] * bt[i]; Skt += bw[i] * bk[i] * bt[i];
        }
        double dd = S * Skk - Sk * Sk;
        if( dd <= 0. ) return 0;
        *P = (S * Skt - Sk * St) / dd;
        *phi = (St - *P * Sk) / S;
        double r2 = 0.;
        for( int i = 0; i < hits; i++ )
        {
            double r = bt[i] - (*phi + bk[i] * *P);
            r2 += bw[i] * r * r;
            res[i] = fabs( r );
        }
        *jit = sqrt( r2 / S );
        *sd = *jit * sqrt( S / dd );
        qsort( res, hits, sizeof *res, cmp_double );
        double cut = fmax( 3. * res[hits / 2], 3. );
        int kept = 0;
        for( int i = 0; i < hits; i++ )
            if( fabs( bt[i] - (*phi + bk[i] * *P) ) <= cut )
            {
                bk[kept] = bk[i]; bt[kept] = bt[i]; bw[kept] = bw[i];
                kept++;
            }
        if( kept == hits ) break;
        hits = kept;
    }
    return hits >= 12 ? hits : 0;
}

enum { RUN_FIRST, RUN_LAST, RUN_LONGEST };

/* where the kick plays: 250 ms levels of the kick band within 9 dB of its loud end, gaps
 * under 2 s bridged, at least 8 s of it; the first, the last or the longest such stretch */
static bool kick_run( const float *low, int64_t n, int which, int64_t *r0, int64_t *r1 )
{
    int64_t q = n / 250;
    if( q < 32 ) return false;
    float *lv = malloc( q * sizeof *lv );
    double *srt = malloc( q * sizeof *srt );
    bool found = false;
    if( lv && srt )
    {
        for( int64_t i = 0; i < q; i++ )
        {
            double e = 0.;
            for( int k = 0; k < 250; k++ ) e += low[i * 250 + k];
            lv[i] = (float)(10. * log10( e / 250. + 1e-12 ));
            srt[i] = lv[i];
        }
        qsort( srt, q, sizeof *srt, cmp_double );
        double thr = srt[(int64_t)(0.9 * (q - 1))] - 9.;
        for( int64_t i = 0; i < q; )
        {
            if( lv[i] < thr ) { i++; continue; }
            int64_t j = i;
            for( ;; )
            {
                if( j < q && lv[j] >= thr ) { j++; continue; }
                bool more = false;
                for( int64_t k = j; k < j + 8 && k < q && !more; k++ ) more = lv[k] >= thr;
                if( !more ) break;
                j++;
            }
            if( j - i >= 32 )
            {
                bool take = !found || which == RUN_LAST
                         || (which == RUN_LONGEST && j - i > (*r1 - *r0) / 250);
                if( take ) { *r0 = i * 250; *r1 = j * 250; found = true; }
                if( which == RUN_FIRST ) break;
            }
            i = j;
        }
    }
    free( lv );
    free( srt );
    return found;
}

/* the beat in a hop series, where its kick plays: tempo from the autocorrelation of kick
 * onsets, phase from the comb that catches most of them, then trimmed least squares through
 * the kicks themselves over 20 s, 40 s, all of it. In hops from the series' start */
static bool beat_track( const float *low, const float *kw, int64_t n0, int which, dj_grid_t *g )
{
    memset( g, 0, sizeof *g );
    int64_t r0 = 0, r1 = 0;
    if( !kick_run( low, n0, which, &r0, &r1 ) ) return false;
    low += r0; kw += r0;
    int64_t n = r1 - r0, m4 = n / 4;
    bool ok = false;
    const int L0 = 75, L1 = 214;              /* 4 ms lags: 200 down to 70 BPM */
    float *o = malloc( n * sizeof *o ), *ko = malloc( n * sizeof *ko ), *s = malloc( n * sizeof *s );
    float *o4 = malloc( m4 * sizeof *o4 );
    double *acf = calloc( 4 * L1 + 8, sizeof *acf );
    double *bk = malloc( 2048 * sizeof *bk ), *bt = malloc( 2048 * sizeof *bt );
    double *bw = malloc( 2048 * sizeof *bw ), *res = malloc( 2048 * sizeof *res );
    if( !o || !ko || !s || !o4 || !acf || !bk || !bt || !bw || !res ) goto out;

    onset( low, n, s, o );
    onset( kw, n, s, ko );
    double mean = 0., omean = 0.;
    for( int64_t i = 0; i < n; i++ ) omean += o[i];
    omean /= n;
    for( int64_t j = 0; j < m4; j++ )
    {
        o4[j] = o[4 * j] + o[4 * j + 1] + o[4 * j + 2] + o[4 * j + 3];
        mean += o4[j];
    }
    mean /= m4;
    for( int64_t j = 0; j < m4; j++ ) o4[j] -= (float)mean;
    for( int l = L0 - 1; l <= 4 * (L1 + 1) && l < m4; l++ )
    {
        double sum = 0.;
        for( int64_t j = 0; j + l < m4; j++ ) sum += (double)o4[j] * o4[j + l];
        acf[l] = sum / (double)(m4 - l);
    }
    int bl = 0;
    double best = -HUGE_VAL;
    for( int l = L0; l <= L1; l++ )
    {
        double sc = tempo_score( acf, l );
        if( sc > best ) { best = sc; bl = l; }
    }
    if( best <= 0. ) goto out;
    double y0 = tempo_score( acf, bl - 1 ), y1 = best, y2 = tempo_score( acf, bl + 1 );
    double den = y0 - 2. * y1 + y2, lf = bl;
    if( den < 0. ) lf += 0.5 * (y0 - y2) / den;
    double P = 4. * lf;

    /* the phase over the first 20 s, onsets smoothed by 7 ms */
    for( int64_t i = 0; i < n; i++ )
    {
        double acc = 0.;
        for( int64_t k = i - 3; k <= i + 3; k++ ) if( k >= 0 && k < n ) acc += o[k];
        s[i] = (float)acc;
    }
    int64_t lim = n < 20000 ? n : 20000;
    double phi = 0., bestp = -1.;
    for( int ph = 0; ph < (int)P; ph++ )
    {
        double sc = 0.;
        for( double t = ph; t < lim - 1; t += P ) sc += s[(int64_t)(t + 0.5)];
        if( sc > bestp ) { bestp = sc; phi = ph; }
    }

    int hits = 0, beats = 0;
    double jit = 0., sd = 0.;
    const int64_t lims[3] = { n < 20000 ? n : 20000, n < 40000 ? n : 40000, n };
    for( int pass = 0; pass < 3; pass++ )
    {
        double win = P / (pass == 0 ? 8. : pass == 1 ? 12. : 16.);
        hits = beat_pass( o, n, lims[pass], 1.5 * omean, win, &P, &phi, &jit, &sd, &beats, bk, bt, bw, res );
        if( hits == 0 ) goto out;
    }

    /* claps and snares sit on 2 and 4: the parity with less of them starts the half bars */
    double ev = 0., od = 0.;
    int ne = 0, no = 0;
    for( int k = 0; ; k++ )
    {
        double c = phi + k * P;
        if( c + P >= n ) break;
        if( c < 0. ) continue;
        float v = 0.f;
        for( int64_t i = (int64_t)c; i < (int64_t)(c + P) && i < n; i++ ) if( ko[i] > v ) v = ko[i];
        if( k & 1 ) { od += v; no++; } else { ev += v; ne++; }
    }
    if( ne && no )
    {
        ev /= ne; od /= no;
        if( fabs( ev - od ) >= 0.08 * fmax( ev, od ) ) g->one = ev <= od ? 0 : 1;
    }
    g->period = P;
    g->phase = r0 + phi;
    g->bpm = 60000. / P;
    g->conf = beats ? (float)hits / beats : 0.f;
    g->jitter = (float)jit;
    g->drift = 64. * sd;
    g->kick_from = (double)r0;
    g->kick_to = (double)r1;
    ok = g->conf >= 0.6f && hits >= 16 && g->jitter <= 12.f && g->drift <= 8.
      && g->bpm >= 70. && g->bpm <= 200.;
out:
    free( o ); free( ko ); free( s ); free( o4 ); free( acf );
    free( bk ); free( bt ); free( bw ); free( res );
    return ok;
}

/* a deck's grid between two of its frames, in its frames */
static bool grid_fit( deck_t *d, int64_t f0, int64_t f1, int which, dj_grid_t *g )
{
    memset( g, 0, sizeof *g );
    int64_t h0 = f0 / DJ_HOP, h1 = f1 / DJ_HOP;
    float *low, *kw, *pk;
    if( !hops_get( d, &h0, &h1, &low, &kw, &pk ) ) return false;
    bool ok = beat_track( low, kw, h1 - h0, which, g );
    free( low ); free( kw ); free( pk );
    if( g->period > 0. )
    {
        g->period *= DJ_HOP;
        g->phase = (h0 + g->phase) * DJ_HOP + DJ_HOP / 2;
        g->kick_from = (h0 + g->kick_from) * DJ_HOP;
        g->kick_to = (h0 + g->kick_to) * DJ_HOP;
    }
    return ok;
}

/* the kick band over one beat, folded on a grid, as 400 bins of log energy, mean removed:
 * what a deck's beat sounds like, wherever its onsets were caught */
static bool beat_profile( deck_t *d, const dj_grid_t *g, double f0, double f1, float *prof )
{
    int64_t h0 = (int64_t)(f0 / DJ_HOP), h1 = (int64_t)(f1 / DJ_HOP);
    float *low, *kw, *pk;
    if( g->period <= 0. || !hops_get( d, &h0, &h1, &low, &kw, &pk ) ) return false;
    double sum[400] = { 0. };
    int cnt[400] = { 0 };
    for( int64_t i = 0; i < h1 - h0; i++ )
    {
        double t = (double)((h0 + i) * DJ_HOP + DJ_HOP / 2) - g->phase;
        double u = t / g->period - floor( t / g->period );
        int k = (int)(u * 400.);
        if( k > 399 ) k = 399;
        sum[k] += low[i];
        cnt[k]++;
    }
    free( low ); free( kw ); free( pk );
    double mean = 0.;
    for( int k = 0; k < 400; k++ )
    {
        if( cnt[k] == 0 ) return false;
        prof[k] = (float)log( sum[k] / cnt[k] + 1e-12 );
        mean += prof[k];
    }
    for( int k = 0; k < 400; k++ ) prof[k] -= (float)(mean / 400.);
    return true;
}

/* where in a profile the energy rises: the attacks */
static void profile_rise( const float *p, float *r )
{
    float sm[400], mean = 0.f;
    for( int k = 0; k < 400; k++ )
    {
        float v = 0.f;
        for( int j = -2; j <= 2; j++ ) v += p[(k + j + 400) % 400];
        sm[k] = v / 5.f;
    }
    for( int k = 0; k < 400; k++ )
    {
        float d = sm[k] - sm[(k + 396) % 400];
        r[k] = d > 0.f ? d : 0.f;
        mean += r[k];
    }
    for( int k = 0; k < 400; k++ ) r[k] -= mean / 400.f;
}

/* how far into its beat the incoming deck's kick sits, past the outgoing deck's: the
 * circular cross-correlation of their profiles, in beats, (-0.5, 0.5]; within 'span' bins
 * of 'around' when span is under 200 */
static double profile_shift( const float *pa, const float *pb, double *peak, int around, int span )
{
    double c[400], na = 0., nb = 0.;
    for( int k = 0; k < 400; k++ ) { na += (double)pa[k] * pa[k]; nb += (double)pb[k] * pb[k]; }
    int best = -1;
    for( int s = 0; s < 400; s++ )
    {
        int off = ((s - around) % 400 + 600) % 400 - 200;
        if( span < 200 && (off < -span || off > span) ) { c[s] = -HUGE_VAL; continue; }
        double v = 0.;
        for( int k = 0; k < 400; k++ ) v += (double)pa[k] * pb[(k + s) % 400];
        c[s] = v;
        if( best < 0 || v > c[best] ) best = s;
    }
    double y0 = c[(best + 399) % 400], y1 = c[best], y2 = c[(best + 1) % 400], den = y0 - 2. * y1 + y2;
    double s = best + (isfinite( y0 ) && isfinite( y2 ) && den < 0. ? 0.5 * (y0 - y2) / den : 0.);
    *peak = na > 0. && nb > 0. ? y1 / sqrt( na * nb ) : 0.;
    double du = s / 400.;
    return du > 0.5 ? du - 1. : du;
}

static double beat_at_or_before( const dj_grid_t *g, double x, bool one )
{
    double k = floor( (x - g->phase) / g->period );
    if( one && ((((long long)k - g->one) % 2) + 2) % 2 ) k -= 1.;
    return g->phase + k * g->period;
}

static double beat_at_or_after( const dj_grid_t *g, double x, bool one )
{
    double k = ceil( (x - g->phase) / g->period );
    if( one && ((((long long)k - g->one) % 2) + 2) % 2 ) k += 1.;
    return g->phase + k * g->period;
}

/* BS.1770 gating over 400 ms blocks: the energy of what is loud enough to count */
static double gated_energy( const float *kw, int64_t n )
{
    const double abs_gate = 1.1724e-7;   /* -70 LUFS */
    double all = 0., sum = 0.;
    int cnt = 0;
    for( int pass = 0; pass < 2; pass++ )
    {
        double rel = pass && cnt ? all / cnt / 10. : 0.;
        int c2 = 0;
        for( int64_t b = 0; b + 400 <= n; b += 100 )
        {
            double e = 0.;
            for( int64_t i = b; i < b + 400; i++ ) e += kw[i];
            e /= 400.;
            if( e <= abs_gate ) continue;
            if( pass == 0 ) { all += e; cnt++; }
            else if( e > rel ) { sum += e; c2++; }
        }
        if( pass == 1 ) return c2 ? sum / c2 : 0.;
        if( cnt == 0 ) return 0.;
    }
    return 0.;
}

static float lufs( double e )
{
    return e > 0. ? (float)(-0.691 + 10. * log10( e )) : -70.f;
}

/* the last frame where the music is within 8 dB of itself, over what the deck holds */
static double music_end( deck_t *d, int64_t written )
{
    int64_t h1 = written / DJ_HOP, h0 = h1 - (int64_t)RING_S * 1000;
    float *low, *kw, *pk;
    if( !hops_get( d, &h0, &h1, &low, &kw, &pk ) ) return (double)written;
    int64_t n = h1 - h0;
    double ref = gated_energy( kw, n ), thr = ref * 0.158;
    double end = (double)written;
    double *pre = malloc( (n + 1) * sizeof *pre );
    if( pre && ref > 0. )
    {
        pre[0] = 0.;
        for( int64_t i = 0; i < n; i++ ) pre[i + 1] = pre[i] + kw[i];
        end = (double)(h0 * DJ_HOP);
        for( int64_t h = n - 1; h >= 0; h-- )
        {
            int64_t a = h - 1500 < 0 ? 0 : h - 1500, b = h + 1500 > n ? n : h + 1500;
            if( (pre[b] - pre[a]) / (double)(b - a) >= thr )
            {
                end = (double)((h0 + h + 1) * DJ_HOP);
                break;
            }
        }
    }
    free( pre ); free( low ); free( kw ); free( pk );
    return end;
}

/* toward the target loudness: at most 3 dB up, 12 down, and never the peaks past -1 dBFS */
static float trim_for( float loud, float peak_db )
{
    float t = TARGET - loud;
    if( t > 3.f ) t = 3.f;
    if( t < -12.f ) t = -12.f;
    if( t > -1.f - peak_db ) t = -1.f - peak_db;
    return t;
}

/* a deck that has read enough: where its music starts, its grid, its loudness */
static void analyse_cue( dj_mix_t *m, deck_t *d )
{
    int64_t h0 = 0, h1 = INT64_MAX;
    float *low = NULL, *kw = NULL, *pk = NULL;
    double fa = 0., cue = 0.;
    float loud = TARGET, peak_db = -1.f, trim_db = 0.f;
    dj_grid_t g;
    bool ok = false;
    memset( &g, 0, sizeof g );
    if( hops_get( d, &h0, &h1, &low, &kw, &pk ) )
    {
        int64_t n = h1 - h0, start = 0;
        double acc = 0.;
        for( int64_t i = 0; i < n; i++ )
        {
            acc += kw[i];
            if( i >= 50 ) acc -= kw[i - 50];
            if( i >= 49 && acc / 50. > 1.2e-5 ) { start = i - 49; break; }
        }
        fa = (double)((h0 + start) * DJ_HOP);
        ok = grid_fit( d, (int64_t)fa, (int64_t)fa + 75 * DJ_RATE, RUN_FIRST, &g );
        int64_t from = start, end = start + 70000 < n ? start + 70000 : n;
        if( ok )
        {
            from = (int64_t)(g.kick_from / DJ_HOP) - h0;
            end = from + 60000 < n ? from + 60000 : n;
        }
        loud = lufs( gated_energy( kw + from, end - from ) );
        float peak = 0.f;
        for( int64_t i = from; i < end; i++ ) if( pk[i] > peak ) peak = pk[i];
        peak_db = peak > 0.f ? 20.f * log10f( peak ) : -1.f;
        trim_db = trim_for( loud, peak_db );
        cue = ok ? beat_at_or_after( &g, fa - g.period / 4., true ) : fa;
        while( ok && cue < 0. ) cue += 2. * g.period;
    }
    free( low ); free( kw ); free( pk );

    vlc_mutex_lock( &m->lock );
    if( d->state == DJ_LOADING )
    {
        if( ok ) d->grid = g;
        else memset( &d->grid, 0, sizeof d->grid );
        d->first_audible = floor( fa );
        d->cue = cue;
        d->loud = loud;
        d->peak_db = peak_db;
        d->trim = d->trim_target = powf( 10.f, trim_db / 20.f );
        d->pos = floor( cue );
        d->state = DJ_CUED;
        vlc_mutex_lock( &d->lock );
        int64_t fl = (int64_t)d->pos - (int64_t)KEEP_S * DJ_RATE;
        if( fl > d->floor ) d->floor = fl;
        vlc_cond_broadcast( &d->cond );
        vlc_mutex_unlock( &d->lock );
    }
    vlc_mutex_unlock( &m->lock );
    if( ok )
        say( m, "cued", "%s: %.2f BPM, %.0f%% on the grid, %.1f ms off, %.1f LUFS, trim %+.1f dB",
             d->t.title ? d->t.title : "?", g.bpm, g.conf * 100., g.jitter, loud, trim_db );
    else
        say( m, "cued", "%s: no steady beat, %.1f LUFS, trim %+.1f dB",
             d->t.title ? d->t.title : "?", loud, trim_db );
}

/*****************************************************************************
 * The mixer, on the output's thread, m->lock held
 *****************************************************************************/
static double bessel_i0( double x )
{
    double sum = 1., term = 1.;
    for( double k = 1.; k < 64.; k += 1. )
    {
        term *= (x / (2. * k)) * (x / (2. * k));
        sum += term;
        if( term < sum * 1e-14 ) break;
    }
    return sum;
}

/* windowed sinc, cut at 21.3 kHz: passes 20 kHz, stops what a 6 % speed-up would fold back;
 * a row per phase, plus one to interpolate toward */
static float *sinc_table( void )
{
    float *t = malloc( (PHASES + 1) * TAPS * sizeof *t );
    if( t == NULL ) return NULL;
    const double fc = 0.8875, beta = 7.86, i0b = bessel_i0( beta );
    for( int p = 0; p <= PHASES; p++ )
    {
        double frac = (double)p / PHASES, sum = 0., row[TAPS];
        for( int k = 0; k < TAPS; k++ )
        {
            double x = (k - HALF + 1) - frac, u = x / HALF;
            double w = fabs( u ) >= 1. ? 0. : bessel_i0( beta * sqrt( 1. - u * u ) ) / i0b;
            double s = fabs( x ) < 1e-12 ? fc : sin( M_PI * fc * x ) / (M_PI * x);
            row[k] = s * w;
            sum += row[k];
        }
        for( int k = 0; k < TAPS; k++ ) t[p * TAPS + k] = (float)(row[k] / sum);
    }
    return t;
}

/* the blend, in beats of a 64-beat mix: in under the bass cut, the swap at 32, out by 64 */
static float blend_in( double b )
{
    return b <= 0. ? 0.f : b < 16. ? 0.84f * (float)sin( M_PI / 2. * b / 16. ) : b < 32. ? 0.84f
         : b < 33. ? 0.84f + 0.16f * (float)(b - 32.) : 1.f;
}
static float blend_out( double b )
{
    return b < 16. ? 1.f : b < 32. ? 1.f - 0.16f * (float)((b - 16.) / 16.)
         : b < 64. ? 0.84f * (float)cos( M_PI / 2. * (b - 32.) / 32. ) : 0.f;
}
static float blend_low_in( double b )  { return b < 32. ? KILL : b < 33. ? KILL * (float)(33. - b) : 0.f; }
static float blend_low_out( double b ) { return b < 32. ? 0.f : b < 33. ? KILL * (float)(b - 32.) : KILL; }

static double trans_end( const trans_t *tr )
{
    return tr->kind == DJ_BLEND ? tr->beats * tr->beat : tr->kind == DJ_ECHO ? 17. * tr->beat : tr->length;
}

/* where the transition puts a deck at mix frame 'now' */
static void automate( const dj_mix_t *m, const deck_t *d, int64_t now, float *gain, float *low,
                      float *send, float *dry )
{
    const trans_t *tr = &m->tr;
    *gain = 1.f; *low = 0.f; *send = 0.f; *dry = 1.f;
    if( !tr->running ) return;
    bool in = d->id == tr->to, out = d->id == tr->from;
    if( !in && !out ) return;
    double t = (double)(now - tr->t0);
    switch( tr->kind )
    {
        case DJ_START:
            if( in ) *gain = t >= tr->length ? 1.f : (float)sin( M_PI / 2. * t / tr->length );
            break;
        case DJ_FADE:
        {
            double x = t / tr->length;
            x = x < 0. ? 0. : x > 1. ? 1. : x;
            *gain = in ? (float)sin( M_PI / 2. * x ) : (float)cos( M_PI / 2. * x );
            break;
        }
        case DJ_BLEND:
        {
            double b = t / tr->beat * 64. / tr->beats;
            *gain = in ? blend_in( b ) : blend_out( b );
            *low = in ? blend_low_in( b ) : blend_low_out( b );
            break;
        }
        case DJ_ECHO:
            if( out )
            {
                double b = t / tr->beat;
                *send = b < 1. ? 1.f : b < 1.0625 ? (float)((1.0625 - b) / 0.0625) : 0.f;
                *dry = b < 1. ? 1.f : b < 1.125 ? (float)((1.125 - b) / 0.125) : 0.f;
            }
            break;
    }
}

/* n frames of a deck at its position and speed; a deck heard alone holds still when its
 * reading falls behind, one in a transition keeps time */
static void deck_fetch( dj_mix_t *m, deck_t *d, float *dst, int n, bool hold )
{
    vlc_mutex_lock( &d->lock );
    int64_t written = d->written;
    bool dead = d->dead;
    vlc_mutex_unlock( &d->lock );
    const int64_t cap = d->cap;
    if( !d->resample )
    {
        int64_t p = (int64_t)d->pos, avail = written - p;
        int got = avail >= n ? n : avail > 0 ? (int)avail : 0;
        for( int k = 0; k < got; )
        {
            int64_t at = (p + k) % cap;
            int run = got - k;
            if( run > cap - at ) run = (int)(cap - at);
            memcpy( dst + 2 * k, d->buf + 2 * at, run * 2 * sizeof (float) );
            k += run;
        }
        if( got < n ) memset( dst + 2 * got, 0, (n - got) * 2 * sizeof (float) );
        d->underrun = got < n && !dead;
        d->pos = (double)(p + (d->underrun && hold ? got : n));
    }
    else
    {
        d->underrun = false;
        for( int i = 0; i < n; i++ )
        {
            int64_t ip = (int64_t)floor( d->pos ), first = ip - HALF + 1;
            if( ip + HALF >= written )
            {
                if( !dead && hold )
                {
                    memset( dst + 2 * i, 0, (n - i) * 2 * sizeof (float) );
                    d->underrun = true;
                    break;
                }
                d->underrun = !dead;
                dst[2 * i] = dst[2 * i + 1] = 0.f;
            }
            else
            {
                double ph = (d->pos - (double)ip) * PHASES;
                int pi = (int)ph;
                float pf = (float)(ph - pi), l = 0.f, r = 0.f;
                const float *h0 = m->sinc + pi * TAPS, *h1 = h0 + TAPS;
                int64_t at = first >= 0 ? first % cap : -1;
                if( at >= 0 && at + TAPS <= cap )
                {
                    const float *s = d->buf + 2 * at;
                    for( int k = 0; k < TAPS; k++ )
                    {
                        float c = h0[k] + pf * (h1[k] - h0[k]);
                        l += c * s[2 * k];
                        r += c * s[2 * k + 1];
                    }
                }
                else
                    for( int k = 0; k < TAPS; k++ )
                    {
                        int64_t f = first + k;
                        if( f < 0 ) continue;
                        float c = h0[k] + pf * (h1[k] - h0[k]);
                        l += c * d->buf[2 * (f % cap)];
                        r += c * d->buf[2 * (f % cap) + 1];
                    }
                dst[2 * i] = l;
                dst[2 * i + 1] = r;
            }
            d->pos += d->snap_left > 0 ? d->snap_rate : d->rate;
            if( d->snap_left > 0 && --d->snap_left == 0 )
            {
                d->pos = floor( d->pos + 0.5 );
                d->rate = 1.;
            }
        }
    }
    vlc_mutex_lock( &d->lock );
    int64_t fl = (int64_t)d->pos - (int64_t)KEEP_S * DJ_RATE;
    if( fl > d->floor ) d->floor = fl;
    vlc_cond_broadcast( &d->cond );
    vlc_mutex_unlock( &d->lock );
}

static void deck_render( dj_mix_t *m, deck_t *d, float *acc, int n )
{
    const trans_t *tr = &m->tr;
    bool in_tr = tr->running && (d->id == tr->from || d->id == tr->to);
    float *tmp = m->tmp, peak = 0.f;
    int i = 0;
    if( m->time + n <= d->start_at ) return;
    if( m->time < d->start_at ) i = (int)(d->start_at - m->time);
    while( i < n )
    {
        int sub = n - i < SUB ? n - i : SUB;
        /* the tempo rides back to the record's own once it is alone; then a whole frame */
        if( !in_tr && d->snap_left == 0 && d->rate != d->rate_target )
        {
            double step = SLEW * sub / DJ_RATE;
            d->rate = fabs( d->rate_target - d->rate ) <= step ? d->rate_target
                    : d->rate + (d->rate < d->rate_target ? step : -step);
            d->resample = true;
        }
        if( !in_tr && d->resample && d->snap_left == 0 && d->rate == 1. && d->rate_target == 1. )
        {
            double frac = d->pos - floor( d->pos );
            if( frac == 0. ) d->resample = false;
            else { d->snap_left = 4800; d->snap_rate = 1. + (1. - frac) / 4800.; }
        }
        float g1, low1, send1, dry1;
        automate( m, d, m->time + i + sub, &g1, &low1, &send1, &dry1 );
        if( d->trim != d->trim_target )   /* remeasured: to its new level at 2 dB a second */
        {
            float k = powf( 10.f, .1f * sub / DJ_RATE );
            d->trim = d->trim < d->trim_target ? fminf( d->trim * k, d->trim_target ) : fmaxf( d->trim / k, d->trim_target );
        }
        g1 *= d->trim;
        deck_fetch( m, d, tmp, sub, !in_tr );

        if( low1 != 0.f || d->shelf_on )
        {
            if( !d->shelf_on ) { bq_clear( &d->shelf[0] ); bq_clear( &d->shelf[1] ); d->shelf_db = 1.f; }
            d->shelf_on = true;
            if( fabsf( low1 - d->shelf_db ) > 0.01f )
            {
                bq_t s0 = d->shelf[0], s1 = d->shelf[1];
                bq_lowshelf( &d->shelf[0], 180., low1 );
                d->shelf[1] = d->shelf[0];
                d->shelf[0].x1 = s0.x1; d->shelf[0].x2 = s0.x2; d->shelf[0].y1 = s0.y1; d->shelf[0].y2 = s0.y2;
                d->shelf[1].x1 = s1.x1; d->shelf[1].x2 = s1.x2; d->shelf[1].y1 = s1.y1; d->shelf[1].y2 = s1.y2;
                d->shelf_db = low1;
            }
            for( int k = 0; k < sub; k++ )
            {
                tmp[2 * k] = (float)bq_run( &d->shelf[0], tmp[2 * k] );
                tmp[2 * k + 1] = (float)bq_run( &d->shelf[1], tmp[2 * k + 1] );
            }
            /* flat again, and settled: out of the path */
            if( low1 == 0.f && fabs( d->shelf[0].x1 - d->shelf[0].y1 ) < 1e-7
             && fabs( d->shelf[1].x1 - d->shelf[1].y1 ) < 1e-7 )
                d->shelf_on = false;
        }
        d->low_db = low1;

        if( send1 > 0.f || d->echo_on )
        {
            float s0 = d->echo_send, r0 = d->dry;
            d->echo_on = true;
            for( int k = 0; k < sub; k++ )
            {
                float f = (float)(k + 1) / sub, sn = s0 + (send1 - s0) * f, dr = r0 + (dry1 - r0) * f;
                int rd = d->echo_w - d->echo_len;
                if( rd < 0 ) rd += ECHO_MAX;
                float el = d->echo[2 * rd], er = d->echo[2 * rd + 1];
                float fl = (float)bq_run( &d->elp[0], bq_run( &d->ehp[0], el ) );
                float fr = (float)bq_run( &d->elp[1], bq_run( &d->ehp[1], er ) );
                d->echo[2 * d->echo_w] = tmp[2 * k] * sn + fl * 0.55f;
                d->echo[2 * d->echo_w + 1] = tmp[2 * k + 1] * sn + fr * 0.55f;
                if( ++d->echo_w == ECHO_MAX ) d->echo_w = 0;
                tmp[2 * k] = tmp[2 * k] * dr + el * 0.8f;
                tmp[2 * k + 1] = tmp[2 * k + 1] * dr + er * 0.8f;
            }
            d->echo_send = send1;
            d->dry = dry1;
        }

        float g0 = d->gain;
        float *o = acc + 2 * i;
        if( g0 == g1 && g1 == 1.f )
            for( int k = 0; k < 2 * sub; k++ ) o[k] += tmp[k];
        else
            for( int k = 0; k < sub; k++ )
            {
                float g = g0 + (g1 - g0) * (float)(k + 1) / sub;
                o[2 * k] += tmp[2 * k] * g;
                o[2 * k + 1] += tmp[2 * k + 1] * g;
            }
        for( int k = 0; k < 2 * sub; k++ )
        {
            float a = fabsf( tmp[k] ) * g1;
            if( a > peak ) peak = a;
        }
        d->gain = g1;
        i += sub;
    }
    d->level = fmaxf( d->level * 0.9f, peak );
    if( d->underrun && !in_tr ) { if( d->starved_since == 0 ) d->starved_since = mdate(); }
    else d->starved_since = 0;
}

static void trans_begin( dj_mix_t *m )
{
    trans_t *tr = &m->tr;
    deck_t *b = &m->deck[tr->to];
    double pos = tr->to_cue;
    if( tr->kind == DJ_BLEND && tr->from >= 0 )
    {
        deck_t *a = &m->deck[tr->from];
        double over = a->pos - tr->at;
        if( over > 0. ) pos += over * tr->to_rate / a->rate;
    }
    if( tr->to_rate == 1. ) pos = floor( pos + 0.5 );
    b->pos = pos;
    b->rate = b->rate_target = tr->to_rate;
    b->resample = tr->to_rate != 1. || pos != floor( pos );
    b->snap_left = 0;
    b->gain = 0.f;
    b->low_db = 0.f;
    b->shelf_on = false;
    b->echo_on = false;
    b->start_at = m->time + (tr->kind == DJ_ECHO ? (int64_t)llround( tr->beat ) : 0);
    b->audible = true;
    b->state = DJ_LIVE;
    if( tr->kind == DJ_ECHO && tr->from >= 0 )
    {
        deck_t *a = &m->deck[tr->from];
        long len = lround( 0.75 * tr->beat );
        a->echo_len = len < 1 ? 1 : len >= ECHO_MAX ? ECHO_MAX - 1 : (int)len;
        memset( a->echo, 0, 2 * ECHO_MAX * sizeof (float) );
        a->echo_w = 0;
        a->echo_send = 0.f;
        a->dry = 1.f;
        for( int c = 0; c < 2; c++ )
        {
            bq_highpass( &a->ehp[c], 250., M_SQRT1_2 ); bq_clear( &a->ehp[c] );
            bq_lowpass( &a->elp[c], 4500., M_SQRT1_2 ); bq_clear( &a->elp[c] );
        }
    }
    vlc_mutex_lock( &b->lock );
    int64_t fl = (int64_t)pos - (int64_t)KEEP_S * DJ_RATE;
    if( fl > b->floor ) b->floor = fl;
    vlc_cond_broadcast( &b->cond );
    vlc_mutex_unlock( &b->lock );
    tr->running = true;
    tr->t0 = m->time;
    if( tr->from < 0 ) { m->live = tr->to; m->cue = -1; }
    m->events |= EV_BEGAN;
}

static void trans_check( dj_mix_t *m )
{
    trans_t *tr = &m->tr;
    if( (double)(m->time - tr->t0) < trans_end( tr ) ) return;
    if( tr->from >= 0 )
    {
        deck_t *a = &m->deck[tr->from];
        a->audible = false;
        a->state = DJ_DONE;
        a->echo_on = false;
    }
    m->deck[tr->to].rate_target = 1.;
    m->live = tr->to;
    m->cue = -1;
    tr->running = tr->armed = false;
    m->events |= EV_DONE;
    vlc_cond_signal( &m->wake );
}

/* look-ahead peak limiter: the gain falls in the 2 ms before a peak above -0.5 dBFS and
 * recovers over 150 ms; below that it is a plain delay, the samples untouched */
static void limit( dj_mix_t *m, float *buf, int n )
{
    const float att = 1.f - expf( -1.f / (0.0005f * DJ_RATE) );
    const float rel = 1.f - expf( -1.f / (0.150f * DJ_RATE) );
    for( int i = 0; i < n; i++ )
    {
        float l = buf[2 * i], r = buf[2 * i + 1];
        float pk = fmaxf( fabsf( l ), fabsf( r ) );
        float need = pk > LIM_CEIL ? LIM_CEIL / pk : 1.f;
        m->lim_need[m->lim_w] = need;
        if( need < 1.f ) m->lim_hot = LIM_LA;
        float lo = 1.f;
        if( m->lim_hot > 0 )
        {
            m->lim_hot--;
            for( int k = 0; k < LIM_LA; k++ ) lo = fminf( lo, m->lim_need[k] );
        }
        float g = m->lim_gain;
        g += (lo - g) * (lo < g ? att : rel);
        if( g > 0.99999f && lo == 1.f ) g = 1.f;
        m->lim_gain = g;
        float dl = m->lim_buf[2 * m->lim_w], dr = m->lim_buf[2 * m->lim_w + 1];
        m->lim_buf[2 * m->lim_w] = l;
        m->lim_buf[2 * m->lim_w + 1] = r;
        if( ++m->lim_w == LIM_LA ) m->lim_w = 0;
        if( g != 1.f ) { dl *= g; dr *= g; }
        buf[2 * i] = dl > LIM_CEIL ? LIM_CEIL : dl < -LIM_CEIL ? -LIM_CEIL : dl;
        buf[2 * i + 1] = dr > LIM_CEIL ? LIM_CEIL : dr < -LIM_CEIL ? -LIM_CEIL : dr;
    }
}

static void mix_run( dj_mix_t *m, float *out, int n )
{
    memset( out, 0, n * 2 * sizeof (float) );
    int done = 0;
    while( done < n )
    {
        int seg = n - done;
        trans_t *tr = &m->tr;
        if( tr->armed && !tr->running )
        {
            int k = 0;
            if( tr->kind != DJ_START && tr->from >= 0 && m->deck[tr->from].audible )
            {
                const deck_t *a = &m->deck[tr->from];
                double left = (tr->at - a->pos) / a->rate;
                k = left <= 0. ? 0 : (int)ceil( left );
            }
            if( k == 0 ) trans_begin( m );
            else if( k < seg ) seg = k;
        }
        for( int i = 0; i < DJ_DECKS; i++ )
            if( m->deck[i].audible ) deck_render( m, &m->deck[i], out + 2 * done, seg );
        m->time += seg;
        done += seg;
        if( tr->running ) trans_check( m );
    }

    float g0 = m->master, g1 = g0 + (m->master_target - g0) * 0.25f;
    if( fabsf( g1 - m->master_target ) < 1e-4f ) g1 = m->master_target;
    if( g0 != 1.f || g1 != 1.f )
        for( int i = 0; i < n; i++ )
        {
            float g = g0 + (g1 - g0) * (float)(i + 1) / n;
            out[2 * i] *= g;
            out[2 * i + 1] *= g;
        }
    m->master = g1;
    limit( m, out, n );
    float peak = 0.f;
    for( int i = 0; i < 2 * n; i++ ) peak = fmaxf( peak, fabsf( out[i] ) );
    m->out_level = fmaxf( m->out_level * 0.9f, peak );
}

/*****************************************************************************
 * djmix://, the mix as a stream the playlist plays
 *****************************************************************************/
struct demux_sys_t
{
    dj_mix_t    *mix;
    es_out_id_t *es;
    int64_t      frames;
};

static void mix_release( dj_mix_t *m );

static int MixDemux( demux_t *demux )
{
    demux_sys_t *sys = demux->p_sys;
    dj_mix_t *m = sys->mix;
    block_t *b = block_Alloc( BLOCK * 2 * sizeof (float) );
    if( b == NULL ) return VLC_DEMUXER_EGENERIC;
    vlc_mutex_lock( &m->lock );
    if( m->out == demux ) mix_run( m, (float *)b->p_buffer, BLOCK );
    else memset( b->p_buffer, 0, b->i_buffer );
    vlc_mutex_unlock( &m->lock );
    mtime_t t0 = sys->frames * CLOCK_FREQ / DJ_RATE, t1 = (sys->frames + BLOCK) * CLOCK_FREQ / DJ_RATE;
    b->i_nb_samples = BLOCK;
    b->i_dts = b->i_pts = VLC_TS_0 + t0;
    b->i_length = t1 - t0;
    es_out_SetPCR( demux->out, b->i_pts );
    es_out_Send( demux->out, sys->es, b );
    sys->frames += BLOCK;
    return VLC_DEMUXER_SUCCESS;
}

static int MixControl( demux_t *demux, int query, va_list args )
{
    demux_sys_t *sys = demux->p_sys;
    switch( query )
    {
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_RATE:
            *va_arg( args, bool * ) = false;
            return VLC_SUCCESS;
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
            *va_arg( args, bool * ) = true;
            return VLC_SUCCESS;
        case DEMUX_SET_PAUSE_STATE:
        {
            bool paused = (bool)va_arg( args, int );
            vlc_mutex_lock( &sys->mix->lock );
            sys->mix->paused = paused;
            vlc_mutex_unlock( &sys->mix->lock );
            return VLC_SUCCESS;
        }
        case DEMUX_GET_PTS_DELAY:
            *va_arg( args, int64_t * ) = INT64_C(300000);
            return VLC_SUCCESS;
        case DEMUX_GET_TIME:
            *va_arg( args, int64_t * ) = sys->frames * CLOCK_FREQ / DJ_RATE;
            return VLC_SUCCESS;
        case DEMUX_GET_LENGTH:
            *va_arg( args, int64_t * ) = 0;
            return VLC_SUCCESS;
        case DEMUX_GET_POSITION:
            *va_arg( args, double * ) = 0.;
            return VLC_SUCCESS;
        default:
            return VLC_EGENERIC;
    }
}

int DjMixOpen( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;
    if( demux->b_preparsing ) return VLC_EGENERIC;
    vlc_mutex_lock( &g_lock );
    dj_mix_t *m = g_mix;
    if( m ) m->refs++;
    vlc_mutex_unlock( &g_lock );
    if( m == NULL )
    {
        msg_Err( demux, "the DJ is not running: djmix:// plays only with dj in extraintf" );
        return VLC_EGENERIC;
    }
    demux_sys_t *sys = calloc( 1, sizeof *sys );
    if( sys == NULL ) { mix_release( m ); return VLC_ENOMEM; }
    es_format_t fmt;
    es_format_Init( &fmt, AUDIO_ES, VLC_CODEC_FL32 );
    fmt.audio.i_format = VLC_CODEC_FL32;
    fmt.audio.i_rate = DJ_RATE;
    fmt.audio.i_channels = 2;
    fmt.audio.i_physical_channels = AOUT_CHANS_STEREO;
    fmt.audio.i_bitspersample = 32;
    fmt.audio.i_blockalign = 8;
    fmt.audio.i_bytes_per_frame = 8;
    fmt.audio.i_frame_length = 1;
    fmt.i_bitrate = DJ_RATE * 64;
    fmt.psz_description = strdup( "DJ" );
    sys->es = es_out_Add( demux->out, &fmt );
    es_format_Clean( &fmt );
    if( sys->es == NULL ) { free( sys ); mix_release( m ); return VLC_EGENERIC; }
    sys->mix = m;
    vlc_mutex_lock( &m->lock );
    m->out = demux;
    m->paused = false;
    vlc_mutex_unlock( &m->lock );
    vlc_cond_signal( &m->wake );
    demux->pf_demux = MixDemux;
    demux->pf_control = MixControl;
    demux->p_sys = sys;
    return VLC_SUCCESS;
}

void DjMixClose( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;
    demux_sys_t *sys = demux->p_sys;
    dj_mix_t *m = sys->mix;
    vlc_mutex_lock( &m->lock );
    if( m->out == demux ) m->out = NULL;
    vlc_mutex_unlock( &m->lock );
    mix_release( m );
    free( sys );
}

/*****************************************************************************
 * The plan, on the engine's thread
 *****************************************************************************/
static void plan( dj_mix_t *m, int urgency )
{
    vlc_mutex_lock( &m->lock );
    int li = m->live, ci = m->cue;
    if( ci < 0 || m->deck[ci].state != DJ_CUED || m->tr.armed )
    {
        vlc_mutex_unlock( &m->lock );
        return;
    }
    deck_t *b = &m->deck[ci];
    if( li < 0 || !m->deck[li].audible )
    {
        bool rhythmic = b->grid.period > 0.;
        m->tr = (trans_t){ .kind = DJ_START, .from = -1, .to = ci, .urgency = urgency,
                           .to_cue = rhythmic ? b->cue : b->first_audible, .to_rate = 1.,
                           .length = (rhythmic ? 0.03 : 1.2) * DJ_RATE, .armed = true };
        m->want = -1;
        snprintf( m->note, sizeof m->note, "start: %s", b->t.title ? b->t.title : "?" );
        vlc_mutex_unlock( &m->lock );
        say( m, "plan", "%s", m->note );
        return;
    }
    deck_t *a = &m->deck[li];
    double ra = a->rate, apos = a->pos;
    vlc_mutex_lock( &a->lock );
    int64_t aw = a->written;
    bool aeof = a->dead;
    vlc_mutex_unlock( &a->lock );
    vlc_mutex_unlock( &m->lock );

    double lead = (urgency == DJ_NOW ? 2. : urgency == DJ_SOON ? 6. : 4.) * DJ_RATE * ra;
    double start_min = apos + lead, mend = -1.;
    if( aeof ) mend = music_end( a, aw );
    if( urgency == DJ_NATURAL && mend < 0. ) return;   /* the end is not in sight yet */

    /* the outgoing beat where the mix will be; a beatless outro moves it earlier */
    double w1 = urgency == DJ_NATURAL ? mend : fmin( (double)aw, start_min + 70. * DJ_RATE );
    double w0 = fmax( apos - 8. * DJ_RATE, w1 - 70. * DJ_RATE );
    dj_grid_t ga;
    int which = urgency == DJ_NATURAL ? RUN_LAST : RUN_LONGEST;
    bool a_ok = grid_fit( a, (int64_t)w0, (int64_t)w1, which, &ga );
    if( !a_ok && urgency == DJ_NATURAL )
    {
        w1 = fmax( apos, mend - 30. * DJ_RATE );
        w0 = fmax( apos - 8. * DJ_RATE, w1 - 60. * DJ_RATE );
        if( w1 - w0 > 15. * DJ_RATE && grid_fit( a, (int64_t)w0, (int64_t)w1, RUN_LAST, &ga ) )
        {
            a_ok = true;
            mend = w1;
        }
    }

    float pa[400], pb[400];
    dj_grid_t gbc;
    vlc_mutex_lock( &m->lock );
    gbc = b->grid;
    vlc_mutex_unlock( &m->lock );
    /* the beats that will meet: the outgoing deck where the blend would start, up to the
     * swap, and the incoming deck where its kick comes in */
    int beats_est = ga.bpm >= 110. ? (urgency == DJ_NOW ? 32 : 64) : 16;
    double half_a = beats_est / 2 * ga.period;
    double at_est = urgency == DJ_NATURAL ? fmin( ga.kick_to - half_a, mend - 2. * half_a ) : start_min;
    if( at_est < start_min ) at_est = start_min;
    bool prof_ok = a_ok && gbc.period > 0.
                && beat_profile( a, &ga, at_est - 12. * DJ_RATE, at_est + half_a, pa )
                && beat_profile( b, &gbc, gbc.kick_from, gbc.kick_from + 12. * DJ_RATE, pb );
    double du = 0., corr = 0., du0 = 0., c2 = 0.;
    if( prof_ok )
    {
        /* the patterns line up the kicks; their attacks then, within 25 ms of that */
        du = profile_shift( pa, pb, &corr, 0, 200 );
        float ra_[400], rb_[400];
        du0 = du;
        profile_rise( pa, ra_ );
        profile_rise( pb, rb_ );
        int around = (int)lround( du * 400. );
        double fine = profile_shift( ra_, rb_, &c2, (around + 400) % 400, (int)(25. * 48. * 400. / ga.period) + 1 );
        if( c2 > 0.2 ) du = fine;
    }

    vlc_mutex_lock( &m->lock );
    if( m->live != li || m->cue != ci || m->tr.armed || b->state != DJ_CUED )
    {
        vlc_mutex_unlock( &m->lock );
        return;
    }
    ra = a->rate;
    start_min = a->pos + lead;
    const dj_grid_t *gb = &b->grid;
    bool b_ok = gb->period > 0.;
    double rb = a_ok && b_ok ? gb->period * ra / ga.period : 1.;
    trans_t tr = { .from = li, .to = ci, .urgency = urgency, .to_rate = 1., .armed = true };
    tr.kind = a_ok && b_ok && fabs( rb - 1. ) <= 0.08 ? DJ_BLEND : a_ok ? DJ_ECHO : DJ_FADE;
    if( tr.kind == DJ_BLEND )
    {
        int beats = ga.bpm >= 110. ? (urgency == DJ_NOW ? 32 : 64) : 16;
        /* the bass swaps halfway, while the outgoing kick still plays; its music ends after */
        double at = urgency == DJ_NATURAL
                  ? fmin( ga.kick_to - beats / 2 * ga.period, mend - beats * ga.period ) : start_min;
        if( at < start_min )
        {
            at = start_min;
            if( urgency == DJ_NATURAL )
            {
                double room = fmin( 2. * (ga.kick_to - at), mend - at ) / ga.period;
                int fit = (int)room / 16 * 16;
                if( fit >= 16 && fit < beats ) beats = fit;
            }
        }
        tr.at = beat_at_or_after( &ga, at, true );
        tr.beats = beats;
        tr.beat = ga.period / ra;
        /* the incoming kick comes in on the swap; an intro shorter than that plays whole */
        double cue = beat_at_or_after( gb, gb->kick_from - beats / 2 * gb->period - gb->period / 4., true );
        tr.to_cue = cue < b->first_audible - gb->period / 4. ? b->cue : cue;
        /* kick on kick, whatever each deck's onsets caught */
        if( prof_ok && corr > 0.3 ) tr.to_cue += du * gb->period;
        tr.to_rate = rb;
    }
    else if( tr.kind == DJ_ECHO )
    {
        /* the cut on the last downbeat the outgoing kick still plays: the echo repeats a beat */
        double cut = urgency == DJ_NATURAL
                   ? beat_at_or_before( &ga, fmin( ga.kick_to, mend ) - ga.period, true )
                   : beat_at_or_after( &ga, start_min + ga.period, true );
        if( cut < start_min + ga.period ) cut = beat_at_or_after( &ga, start_min + ga.period, true );
        tr.at = cut - ga.period;
        tr.beat = ga.period / ra;
        tr.beats = 16;
        /* the next one drops where its kick comes in */
        tr.to_cue = b_ok ? beat_at_or_after( gb, gb->kick_from - gb->period / 4., true ) : b->first_audible;
    }
    else
    {
        double len = (urgency == DJ_NOW ? 3. : (a->t.endless || b->t.endless) ? 8. : 6.) * DJ_RATE;
        double at = urgency == DJ_NATURAL ? mend - len * ra / 2. : start_min;
        tr.at = at < start_min ? start_min : at;
        tr.length = len;
        tr.to_cue = b->first_audible;
    }
    a->rate_target = a->rate;   /* the outgoing tempo holds through the mix */
    a->grid = ga;
    m->tr = tr;
    m->want = -1;
    if( tr.kind == DJ_BLEND )
        snprintf( m->note, sizeof m->note, "blend %d beats: %.2f into %.2f BPM, %+.2f %%, kicks %+.0f ms (%.2f), attacks %+.0f ms (%.2f), in %.0f s",
                  tr.beats, gb->bpm, ga.bpm * ra, (rb - 1.) * 100., du0 * gb->period / 48., corr,
                  du * gb->period / 48., c2, (tr.at - a->pos) / ra / DJ_RATE );
    else if( tr.kind == DJ_ECHO )
        snprintf( m->note, sizeof m->note, "echo out of %.2f BPM%s, in %.0f s", ga.bpm * ra,
                  b_ok ? ", the next on its own beat" : "", (tr.at - a->pos) / ra / DJ_RATE );
    else
        snprintf( m->note, sizeof m->note, "fade %.0f s, in %.0f s", tr.length / DJ_RATE,
                  (tr.at - a->pos) / ra / DJ_RATE );
    char note[sizeof m->note];
    strcpy( note, m->note );
    vlc_mutex_unlock( &m->lock );
    say( m, "plan", "%s", note );
}

/*****************************************************************************
 * The engine's thread
 *****************************************************************************/
/* a record with no steady beat where it was cued may find one further in: a grid for the screen
   and the bar count, tried every half minute on what the deck has read ahead; the planner fits
   its own where the records meet */
static void regrid( dj_mix_t *m )
{
    mtime_t now = mdate();
    for( int i = 0; i < DJ_DECKS; i++ )
    {
        deck_t *d = &m->deck[i];
        vlc_mutex_lock( &m->lock );
        bool want = d->state == DJ_LIVE && d->grid.period <= 0. && now - d->regrid_at > 30 * CLOCK_FREQ;
        int64_t from = (int64_t)d->pos;
        if( want ) d->regrid_at = now;
        vlc_mutex_unlock( &m->lock );
        if( !want ) continue;
        vlc_mutex_lock( &d->lock );
        int64_t to = d->written;
        vlc_mutex_unlock( &d->lock );
        if( to - from > 75 * DJ_RATE ) to = from + 75 * DJ_RATE;
        dj_grid_t g;
        if( to - from < 20 * DJ_RATE || !grid_fit( d, from, to, RUN_LONGEST, &g ) ) continue;
        /* its loudness was the intro's: now the kick's, a minute of it */
        int64_t h0 = (int64_t)(g.kick_from / DJ_HOP), h1 = h0 + 60000;
        float *low, *kw, *pk, loud = 0.f, peak = 0.f, trim_db = 0.f;
        bool heard = hops_get( d, &h0, &h1, &low, &kw, &pk ) && h1 - h0 > 8000;
        if( heard )
        {
            loud = lufs( gated_energy( kw, h1 - h0 ) );
            for( int64_t k = 0; k < h1 - h0; k++ ) if( pk[k] > peak ) peak = pk[k];
            trim_db = trim_for( loud, peak > 0.f ? 20.f * log10f( peak ) : -1.f );
        }
        free( low ); free( kw ); free( pk );
        vlc_mutex_lock( &m->lock );
        bool set = d->state == DJ_LIVE && d->grid.period <= 0.;
        if( set )
        {
            d->grid = g;
            if( heard )
            {
                d->loud = loud;
                d->trim_target = powf( 10.f, trim_db / 20.f );
            }
        }
        vlc_mutex_unlock( &m->lock );
        if( set ) say( m, "grid", "deck %c: %.2f BPM, %.0f%% on the grid, %.0f s in; %.1f LUFS, trim %+.1f dB",
                       'A' + i, g.bpm, g.conf * 100., (double)from / DJ_RATE, heard ? loud : 0.f, trim_db );
    }
}

static void mix_tick( dj_mix_t *m )
{
    /* a clear, then a cue to load into the deck nobody hears */
    vlc_mutex_lock( &m->lock );
    if( m->clear )
    {
        for( int i = 0; i < DJ_DECKS; i++ )
            if( m->deck[i].state != DJ_EMPTY ) { m->deck[i].audible = false; m->deck[i].state = DJ_DONE; }
        m->live = m->cue = -1;
        m->tr.armed = m->tr.running = false;
        m->clear = false;
    }
    int target = -1;
    dj_track_t t;
    memset( &t, 0, sizeof t );
    if( m->has_pending )
    {
        bool cue_busy = m->cue >= 0 && m->tr.running && m->tr.to == m->cue;
        if( m->cue >= 0 && !cue_busy && m->deck[m->cue].state != DJ_LIVE )
        {
            target = m->cue;
            m->deck[target].state = DJ_EMPTY;
            m->cue = -1;
            if( m->tr.armed && !m->tr.running ) m->tr.armed = false;
        }
        else if( !cue_busy )
            for( int i = 0; i < DJ_DECKS && target < 0; i++ )
                if( i != m->live && m->deck[i].state == DJ_EMPTY ) target = i;
        if( target >= 0 )
        {
            t = m->pending;
            memset( &m->pending, 0, sizeof m->pending );
            m->has_pending = false;
        }
    }
    vlc_mutex_unlock( &m->lock );
    if( target >= 0 )
    {
        deck_t *d = &m->deck[target];
        if( d->input || d->buf ) deck_unload( d );
        char *title = t.title ? strdup( t.title ) : NULL;
        bool ok = deck_load( m, d, &t );
        vlc_mutex_lock( &m->lock );
        if( ok ) { d->state = DJ_LOADING; m->cue = target; }
        vlc_mutex_unlock( &m->lock );
        say( m, ok ? "loading" : "cannot load", "deck %c: %s", 'A' + target, title ? title : "?" );
        free( title );
    }

    for( int i = 0; i < DJ_DECKS; i++ )
    {
        deck_t *d = &m->deck[i];
        vlc_mutex_lock( &m->lock );
        int st = d->state;
        bool out = st == DJ_DONE || st == DJ_FAILED;
        if( out )
        {
            d->state = DJ_EMPTY;
            d->audible = false;
            if( m->live == i ) m->live = -1;
            if( m->cue == i ) m->cue = -1;
        }
        vlc_mutex_unlock( &m->lock );
        if( out )
        {
            if( st == DJ_FAILED ) say( m, "deck failed", "%s", d->t.title ? d->t.title : "?" );
            deck_unload( d );
            continue;
        }
        if( st != DJ_LOADING ) continue;
        vlc_mutex_lock( &d->lock );
        int64_t w = d->written;
        bool dead = d->dead, failed = d->failed;
        vlc_mutex_unlock( &d->lock );
        mtime_t age = mdate() - d->loaded_at;
        if( (failed && w < DJ_RATE) || (dead && w < DJ_RATE) || (age > 45 * CLOCK_FREQ && w < DJ_RATE) )
        {
            vlc_mutex_lock( &m->lock );
            d->state = DJ_FAILED;
            if( m->cue == i ) m->cue = -1;
            vlc_mutex_unlock( &m->lock );
            continue;
        }
        if( dead || w >= (int64_t)(RING_S - KEEP_S - 2) * DJ_RATE
         || (d->t.endless && w >= 15 * DJ_RATE)
         || (age > 30 * CLOCK_FREQ && w >= 10 * DJ_RATE) )
            analyse_cue( m, d );
    }

    /* the live deck played out, or starved */
    vlc_mutex_lock( &m->lock );
    int urgency = -1;
    if( m->live >= 0 && !m->tr.running )
    {
        deck_t *a = &m->deck[m->live];
        vlc_mutex_lock( &a->lock );
        bool drained = a->dead && a->pos >= (double)a->written - 1.;
        vlc_mutex_unlock( &a->lock );
        if( drained )
        {
            a->audible = false;
            a->state = DJ_DONE;
            m->live = -1;
            m->tr.armed = false;
        }
        else if( a->starved_since && mdate() - a->starved_since > 8 * CLOCK_FREQ )
            urgency = DJ_NOW;
    }
    bool ready = m->cue >= 0 && m->deck[m->cue].state == DJ_CUED && !m->tr.armed;
    if( ready )
    {
        if( m->want > urgency ) urgency = m->want;
        if( m->live < 0 ) urgency = DJ_NOW;
        else if( urgency < 0 && m->deck[m->live].dead ) urgency = DJ_NATURAL;
    }
    unsigned ev = m->events;
    m->events = 0;
    char note[sizeof m->note];
    strcpy( note, m->note );
    int live = m->live;
    char *title = live >= 0 && m->deck[live].t.title ? strdup( m->deck[live].t.title ) : NULL;
    vlc_mutex_unlock( &m->lock );

    if( ev & EV_BEGAN ) say( m, "mixing", "%s", note );
    if( ev & EV_DONE ) say( m, "mixed in", "%s", title ? title : "?" );
    free( title );
    if( ready && urgency >= 0 ) plan( m, urgency );
}

static void *MixThread( void *data )
{
    dj_mix_t *m = data;
    vlc_mutex_lock( &m->lock );
    while( !m->quit )
    {
        vlc_cond_timedwait( &m->wake, &m->lock, mdate() + CLOCK_FREQ / 10 );
        if( m->quit ) break;
        vlc_mutex_unlock( &m->lock );
        mix_tick( m );
        regrid( m );
        vlc_mutex_lock( &m->lock );
    }
    vlc_mutex_unlock( &m->lock );
    return NULL;
}

/*****************************************************************************
 * The engine, for the DJ and its deck window
 *****************************************************************************/
static void mix_release( dj_mix_t *m )
{
    vlc_mutex_lock( &g_lock );
    bool last = --m->refs == 0;
    vlc_mutex_unlock( &g_lock );
    if( !last ) return;
    for( int i = 0; i < DJ_DECKS; i++ )
    {
        vlc_cond_destroy( &m->deck[i].cond );
        vlc_mutex_destroy( &m->deck[i].lock );
    }
    vlc_cond_destroy( &m->wake );
    vlc_mutex_destroy( &m->lock );
    free( m->sinc );
    free( m->tmp );
    free( m );
}

dj_mix_t *dj_mix_New( vlc_object_t *owner, dj_log_cb log, void *opaque )
{
    dj_mix_t *m = calloc( 1, sizeof *m );
    if( m == NULL ) return NULL;
    m->sinc = sinc_table();
    m->tmp = malloc( SUB * 2 * sizeof (float) );
    if( m->sinc == NULL || m->tmp == NULL )
    {
        free( m->sinc ); free( m->tmp ); free( m );
        return NULL;
    }
    m->owner = owner;
    m->refs = 1;
    m->log = log;
    m->opaque = opaque;
    vlc_mutex_init( &m->lock );
    vlc_cond_init( &m->wake );
    for( int i = 0; i < DJ_DECKS; i++ )
    {
        m->deck[i].mix = m;
        m->deck[i].id = i;
        m->deck[i].rate = m->deck[i].rate_target = 1.;
        m->deck[i].trim = m->deck[i].trim_target = 1.f;
        vlc_mutex_init( &m->deck[i].lock );
        vlc_cond_init( &m->deck[i].cond );
    }
    m->live = m->cue = m->want = -1;
    m->master = m->master_target = m->lim_gain = 1.f;
    for( int i = 0; i < LIM_LA; i++ ) m->lim_need[i] = 1.f;
    if( vlc_clone( &m->thread, MixThread, m, VLC_THREAD_PRIORITY_LOW ) == 0 )
        m->started = true;
    else
    {
        mix_release( m );
        return NULL;
    }
    vlc_mutex_lock( &g_lock );
    if( g_mix == NULL ) g_mix = m;
    vlc_mutex_unlock( &g_lock );
    return m;
}

/* the decks out now; an output still open mixes silence until it closes */
void dj_mix_Delete( dj_mix_t *m )
{
    vlc_mutex_lock( &g_lock );
    if( g_mix == m ) g_mix = NULL;
    vlc_mutex_unlock( &g_lock );
    vlc_mutex_lock( &m->lock );
    m->quit = true;
    vlc_cond_signal( &m->wake );
    vlc_mutex_unlock( &m->lock );
    if( m->started ) vlc_join( m->thread, NULL );
    vlc_mutex_lock( &m->lock );
    for( int i = 0; i < DJ_DECKS; i++ )
    {
        m->deck[i].audible = false;
        m->deck[i].state = DJ_EMPTY;
    }
    m->live = m->cue = -1;
    m->tr.armed = m->tr.running = false;
    m->log = NULL;
    vlc_mutex_unlock( &m->lock );
    for( int i = 0; i < DJ_DECKS; i++ ) deck_unload( &m->deck[i] );
    dj_track_Clean( &m->pending );
    mix_release( m );
}

void dj_mix_Cue( dj_mix_t *m, const dj_track_t *t )
{
    vlc_mutex_lock( &m->lock );
    dj_track_Clean( &m->pending );
    dj_track_Copy( &m->pending, t );
    m->has_pending = true;
    vlc_cond_signal( &m->wake );
    vlc_mutex_unlock( &m->lock );
}

void dj_mix_Next( dj_mix_t *m, int urgency )
{
    vlc_mutex_lock( &m->lock );
    if( urgency > m->want ) m->want = urgency;
    if( m->tr.armed && !m->tr.running && urgency > m->tr.urgency ) m->tr.armed = false;
    vlc_cond_signal( &m->wake );
    vlc_mutex_unlock( &m->lock );
}

void dj_mix_Clear( dj_mix_t *m )
{
    vlc_mutex_lock( &m->lock );
    m->clear = true;
    dj_track_Clean( &m->pending );
    m->has_pending = false;
    m->want = -1;
    vlc_cond_signal( &m->wake );
    vlc_mutex_unlock( &m->lock );
}

void dj_mix_Duck( dj_mix_t *m, float db )
{
    vlc_mutex_lock( &m->lock );
    m->master_target = powf( 10.f, db / 20.f );
    vlc_mutex_unlock( &m->lock );
}

static float db_of( float a )
{
    return a > 1e-6f ? 20.f * log10f( a ) : -120.f;
}

static void deck_info( const dj_mix_t *m, deck_t *d, dj_deck_info_t *o )
{
    o->state = d->state;
    if( d->state == DJ_EMPTY ) return;
    dj_track_Copy( &o->t, &d->t );
    vlc_mutex_lock( &d->lock );
    int64_t w = d->written;
    bool dead = d->dead;
    vlc_mutex_unlock( &d->lock );
    double heard = d->audible ? d->pos - LATENCY_S * DJ_RATE * d->rate : d->pos;
    o->pos = d->pos;
    o->pos_s = heard > 0. ? heard / DJ_RATE : 0.;
    o->length_s = dead ? (double)w / DJ_RATE : d->t.length_ms / 1000.;
    o->ahead_s = ((double)w - d->pos) / DJ_RATE;
    o->eof = dead;
    o->rate = d->rate;
    o->gain = d->audible ? d->gain : 0.f;
    o->trim_db = db_of( d->trim );
    o->low_db = d->low_db;
    o->level_db = db_of( d->level );
    o->loud = d->loud;
    o->grid = d->grid;
    (void)m;
    if( d->item == NULL ) return;
    if( o->length_s <= 0. && !d->t.endless )   /* a file the crate did not time: its demuxer has */
    {
        mtime_t dur = input_item_GetDuration( d->item );
        if( dur > 0 ) o->length_s = (double)dur / CLOCK_FREQ;
    }
    o->now_playing = input_item_GetNowPlaying( d->item );
    vlc_mutex_lock( &d->item->lock );
    for( int i = 0; i < d->item->i_es; i++ )
    {
        const es_format_t *f = d->item->es[i];
        if( f->i_cat != AUDIO_ES ) continue;
        vlc_fourcc_t c = f->i_codec;
        bool pcm = c == VLC_CODEC_S16L || c == VLC_CODEC_S16B || c == VLC_CODEC_S24L || c == VLC_CODEC_S24B
                || c == VLC_CODEC_S32L || c == VLC_CODEC_S32B || c == VLC_CODEC_F32L || c == VLC_CODEC_F32B;
        const char *name = c == VLC_CODEC_FLAC ? "FLAC" : c == VLC_CODEC_ALAC ? "ALAC"
                         : c == VLC_CODEC_MP4A ? "AAC" : c == VLC_CODEC_MPGA || c == VLC_CODEC_MP3 ? "MP3"
                         : c == VLC_CODEC_VORBIS ? "Vorbis" : c == VLC_CODEC_OPUS ? "Opus"
                         : c == VLC_CODEC_WAVPACK ? "WavPack" : pcm ? "PCM" : NULL;
        if( name ) strcpy( o->codec, name );
        else { vlc_fourcc_to_char( c, o->codec ); o->codec[4] = '\0'; }
        o->src_rate = f->audio.i_rate;
        o->kbps = f->i_bitrate > 0 ? (int)(f->i_bitrate / 1000) : 0;
        o->lossless = pcm || c == VLC_CODEC_FLAC || c == VLC_CODEC_ALAC || c == VLC_CODEC_WAVPACK || c == VLC_CODEC_TTA;
        o->src_bits = o->lossless ? f->audio.i_bitspersample : 0;
        break;
    }
    if( o->kbps == 0 && d->item->p_stats )
        o->kbps = (int)lroundf( d->item->p_stats->f_demux_bitrate * 8000.f );
    vlc_mutex_unlock( &d->item->lock );
}

void dj_mix_Status( dj_mix_t *m, dj_status_t *s )
{
    memset( s, 0, sizeof *s );
    vlc_mutex_lock( &m->lock );
    const trans_t *tr = &m->tr;
    s->live = m->live;
    s->cue = m->cue;
    s->loading = m->has_pending;
    s->armed = tr->armed;
    s->running = tr->running;
    s->kind = tr->armed || tr->running ? tr->kind : DJ_NONE;
    s->beats = tr->beats;
    s->from = s->kind != DJ_NONE ? tr->from : -1;
    s->to = s->kind != DJ_NONE ? tr->to : -1;
    if( s->from >= 0 )
    {
        s->at_s = tr->at / DJ_RATE;
        double len = tr->kind == DJ_BLEND || tr->kind == DJ_ECHO ? tr->beats * tr->beat : tr->length;
        s->span_s = len * m->deck[tr->from].rate / DJ_RATE;
    }
    if( tr->running )
    {
        double end = trans_end( tr );
        s->progress = end > 0. ? fmin( 1., (double)(m->time - tr->t0) / end ) : 1.;
    }
    else if( tr->armed && tr->from >= 0 )
    {
        const deck_t *a = &m->deck[tr->from];
        s->starts_in_s = (tr->at - a->pos) / a->rate / DJ_RATE + LATENCY_S;
    }
    float ga = m->deck[0].audible ? m->deck[0].gain : 0.f, gb = m->deck[1].audible ? m->deck[1].gain : 0.f;
    s->xfade = ga + gb > 1e-6f ? (gb - ga) / (ga + gb) : 0.f;
    s->attached = m->out != NULL;
    s->paused = m->paused;
    s->master_db = db_of( m->master );
    s->gr_db = db_of( m->lim_gain );
    s->out_db = db_of( m->out_level );
    s->mixed_s = (double)m->time / DJ_RATE;
    strcpy( s->note, m->note );
    for( int i = 0; i < DJ_DECKS; i++ ) deck_info( m, &m->deck[i], &s->deck[i] );
    vlc_mutex_unlock( &m->lock );
}

void dj_mix_StatusClean( dj_status_t *s )
{
    for( int i = 0; i < DJ_DECKS; i++ )
    {
        dj_track_Clean( &s->deck[i].t );
        free( s->deck[i].now_playing );
        s->deck[i].now_playing = NULL;
    }
}

void dj_mix_Wave( dj_mix_t *m, int di, double from_s, double to_s, int n, float *low, float *full )
{
    for( int c = 0; c < n; c++ ) low[c] = full[c] = -1.f;
    if( di < 0 || di >= DJ_DECKS || n <= 0 || to_s <= from_s ) return;
    deck_t *d = &m->deck[di];
    double h0 = from_s * DJ_RATE / DJ_HOP, span = (to_s - from_s) * DJ_RATE / DJ_HOP / n;
    vlc_mutex_lock( &d->lock );
    if( span >= OV_HOPS / 2 && d->ov )
        for( int c = 0; c < n; c++ )
        {
            int64_t a = (int64_t)floor( (h0 + c * span) / OV_HOPS ), b = (int64_t)floor( (h0 + (c + 1) * span) / OV_HOPS );
            if( b <= a ) b = a + 1;
            if( a < 0 ) a = 0;
            if( b > d->ov_n ) b = d->ov_n;
            if( a >= b ) continue;
            float ml = 0.f, mk = 0.f;
            for( int64_t i = a; i < b; i++ )
            {
                ml = fmaxf( ml, d->ov[2 * i] );
                mk = fmaxf( mk, d->ov[2 * i + 1] );
            }
            low[c] = ml;
            full[c] = mk;
        }
    else if( d->env_low )
    {
        int64_t lo = d->hops - d->hop_cap + 1;
        if( lo < 0 ) lo = 0;
        for( int c = 0; c < n; c++ )
        {
            int64_t a = (int64_t)floor( h0 + c * span ), b = (int64_t)floor( h0 + (c + 1) * span );
            if( b <= a ) b = a + 1;
            if( a < lo ) a = lo;
            if( b > d->hops ) b = d->hops;
            if( a >= b ) continue;
            float ml = 0.f, mk = 0.f;
            for( int64_t h = a; h < b; h++ )
            {
                int64_t k = h % d->hop_cap;
                if( d->env_low[k] > ml ) ml = d->env_low[k];
                if( d->env_k[k] > mk ) mk = d->env_k[k];
            }
            low[c] = sqrtf( ml );
            full[c] = sqrtf( mk );
        }
    }
    vlc_mutex_unlock( &d->lock );
}
