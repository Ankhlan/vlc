/*****************************************************************************
 * dj.h : the DJ's decks, between the DJ (dj.c), its mixer (dj_mix.c) and the
 *        deck window (dj_ui.c)
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

#ifndef VLC_CONTROL_DJ_H
#define VLC_CONTROL_DJ_H

#define DJ_RATE   48000   /* the mix: float stereo at the device's own rate */
#define DJ_HOP    48      /* analysis hop: a millisecond */
#define DJ_DECKS  2
#define DJ_MIX_URI "djmix://live"

typedef struct
{
    char   *url, *title, *artist, *label;
    int64_t length_ms;    /* 0: unknown */
    bool    endless;      /* a station */
    bool    requested;
    int     block, pick;  /* where in the programme it came from; pick -1 for a request */
} dj_track_t;

void dj_track_Copy( dj_track_t *dst, const dj_track_t *src );
void dj_track_Clean( dj_track_t * );

enum { DJ_NATURAL, DJ_SOON, DJ_NOW };   /* how soon the next track comes in */
enum { DJ_EMPTY, DJ_LOADING, DJ_CUED, DJ_LIVE, DJ_DONE, DJ_FAILED };
enum { DJ_NONE, DJ_START, DJ_BLEND, DJ_FADE, DJ_ECHO };

typedef struct
{
    double period;        /* deck frames a beat; 0: no grid */
    double phase;         /* a beat, as a deck frame */
    double bpm;
    float  conf;          /* share of the grid's beats with a kick on them */
    float  jitter;        /* ms: how far the kicks sit off the grid */
    int    one;           /* parity of the beats that start a half bar */
    double drift;         /* ms the grid may wander over a 64-beat blend */
    double kick_from, kick_to;   /* deck frames: the stretch with the kick on, where it was fitted */
} dj_grid_t;

typedef struct
{
    int        state;
    dj_track_t t;                 /* copies, freed by dj_mix_StatusClean */
    char      *now_playing;       /* a station's own title */
    double     pos_s, length_s, ahead_s;   /* heard, whole (0: unknown), read ahead */
    double     pos;               /* the mixer's read position, deck frames */
    bool       eof;
    double     rate;              /* 1: the record's own speed */
    float      gain, trim_db, low_db, level_db;
    float      loud;              /* LUFS of what was analysed */
    dj_grid_t  grid;
    char       codec[8];
    unsigned   src_rate, src_bits;   /* bits only for lossless */
    int        kbps;
    bool       lossless;
} dj_deck_info_t;

typedef struct
{
    int     live, cue;            /* deck, or -1 */
    bool    loading;              /* a cue on its way */
    int     kind;                 /* the transition in hand */
    bool    armed, running;
    int     beats;
    double  progress;             /* 0..1 once running */
    double  starts_in_s;          /* until it begins, armed */
    float   xfade;                /* -1 all deck A .. +1 all deck B */
    int     from, to;             /* the decks it takes out and brings in, or -1 */
    double  at_s, span_s;         /* where it begins on the outgoing record, and how long, its seconds */
    bool    attached, paused;
    float   master_db, gr_db, out_db;
    double  mixed_s;
    char    note[200];            /* the last plan, in words */
    dj_deck_info_t deck[DJ_DECKS];
} dj_status_t;

typedef struct dj_mix dj_mix_t;
typedef void (*dj_log_cb)( void *opaque, const char *verb, const char *what );

dj_mix_t *dj_mix_New( vlc_object_t *owner, dj_log_cb, void *opaque );
void dj_mix_Delete( dj_mix_t * );
void dj_mix_Cue( dj_mix_t *, const dj_track_t * );   /* the next track, into the free deck */
void dj_mix_Next( dj_mix_t *, int urgency );          /* bring the cued track in */
void dj_mix_Clear( dj_mix_t * );                      /* both decks out */
void dj_mix_Duck( dj_mix_t *, float db );
void dj_mix_Status( dj_mix_t *, dj_status_t * );
void dj_mix_StatusClean( dj_status_t * );
/* the record from from_s to to_s, its own seconds, in n columns of amplitude: kick band and
   K-weighted; -1 where nothing is read yet or no longer kept */
void dj_mix_Wave( dj_mix_t *, int deck, double from_s, double to_s, int n, float *low, float *full );

int  DjMixOpen( vlc_object_t * );     /* access_demux djmix:// */
void DjMixClose( vlc_object_t * );
int  DjDeckOpen( vlc_object_t * );    /* sout stream djdeck */
void DjDeckClose( vlc_object_t * );

/* what the DJ shows the deck window; refreshed on the DJ's thread */
typedef struct
{
    bool on_air, busy;
    int  volume, voice;
    char block[64], next_block[64], next_at[8], pick[160], said[240], error[240];
    char next[6][200];
    int  n_next;
    char moods[24][48];
    int  n_moods;
} dj_brain_t;

typedef struct dj_ui dj_ui_t;
typedef void (*dj_brain_cb)( void *opaque, dj_brain_t * );
typedef bool (*dj_command_cb)( void *opaque, const char *cmd );

dj_ui_t *dj_ui_New( vlc_object_t *, dj_mix_t *, dj_brain_cb, dj_command_cb, void *opaque,
                    const char *theme_path );
void dj_ui_Show( dj_ui_t * );
void dj_ui_Delete( dj_ui_t * );

#endif
