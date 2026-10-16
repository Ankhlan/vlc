/*****************************************************************************
 * dj.c : the DJ, inside VLC
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
 * The DJ, four modules in one plugin.
 *
 * "dj", an interface (--extraintf dj), is the DJ. It keeps a programme on air by itself:
 * blocks of the day, each a rotation of picks -- a station, a SoundCloud search or set, an
 * archive.org set, a radio-browser tag. Its music is one item in the playlist, djmix://,
 * the mix of two decks: the next track is always cued on the free deck, and comes in as a
 * club DJ brings it in, beatmatched under a bass swap, echoed out on a downbeat where the
 * tempos will not meet, faded where there is no beat to hold. The music never stops for
 * the DJ and the DJ never talks between tracks; with the voice on, it speaks over the
 * music. A station rotates after rotate_minutes, a block's music comes in at the block's
 * hour, a request holds until the next block. When the human plays something else, or
 * stops, the DJ waits. The programme is dj-programme.json in VLC's config directory.
 *
 * Its verbs answer JSON on VLC's HTTP server under /dj/ (http-host, http-port,
 * http-password, shared with the lua http interface), and through the playlist variable
 * "dj-command", answered in "dj-reply": that is how View > DJ and the deck window talk to
 * it. VLC kills an extension that blocks for ten seconds, so through the variable only
 * state and programme answer at once; every other verb is queued for the DJ's own thread.
 *
 * "djmix", an access_demux, and "djdeck", a stream output, are the mixer's (dj_mix.c): the
 * mix as a stream, and the end of each deck's decoding chain. The deck window is dj_ui.c.
 *
 * "soundcloud", an access (soundcloud://tracks/<id>, soundcloud://resolve?url=<page>), asks
 * SoundCloud for a track's stream when the track plays and redirects there: a signed stream
 * URL expires, so it is fetched as late as possible. SoundCloud serves AAC in
 * fragmented-MP4 HLS now, which the adaptive demuxer plays.
 *
 * Every outside fetch is a vlc_stream, so it honours VLC's http-proxy.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef _WIN32
# include <winsock2.h>   /* before anything pulls in windows.h */
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_access.h>
#include <vlc_interrupt.h>
#include <vlc_httpd.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_memstream.h>
#include <vlc_configuration.h>
#include <vlc_charset.h>
#include <vlc_rand.h>
#include <vlc_fs.h>

#include <ctype.h>
#include <time.h>

#include "../misc/webservices/json.h"
#include "../misc/webservices/json_helper.h"
#include "dj.h"

#include <math.h>

#ifdef _WIN32
# define COBJMACROS
# include <windows.h>
# include <objbase.h>
# include <sapi.h>
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );
static int  ScOpen( vlc_object_t * );

static char *do_deck( intf_sys_t * );

#define DUCK_TEXT N_("Music under the voice")
#define DUCK_LONGTEXT N_("Percent of the music's level while the DJ speaks over it.")
#define AIR_TEXT N_("On air")
#define AIR_LONGTEXT N_("Run the programme: start the block's music when VLC opens with " \
    "nothing to play, rotate, change block on the hour. Off, the DJ only answers requests.")
#define PROG_TEXT N_("Programme")
#define PROG_LONGTEXT N_("The programme file; dj-programme.json in VLC's config directory if empty.")
#define DECK_TEXT N_("Deck window")
#define DECK_LONGTEXT N_("Open the DJ's deck window when VLC starts; View > DJ opens it any time.")
#define THEME_TEXT N_("Deck theme")
#define THEME_LONGTEXT N_("The deck window's theme file; dj-theme.json in VLC's config directory if empty.")

vlc_module_begin ()
    set_shortname( "DJ" )
    set_description( N_("DJ: two decks mixed like a club DJ, a programme on air by itself") )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
    set_capability( "interface", 0 )
    set_callbacks( Open, Close )
    add_shortcut( "dj" )
    add_integer( "dj-duck", 30, DUCK_TEXT, DUCK_LONGTEXT, true )
    add_bool( "dj-on-air", true, AIR_TEXT, AIR_LONGTEXT, false )
    add_string( "dj-programme", NULL, PROG_TEXT, PROG_LONGTEXT, true )
    add_bool( "dj-deck", true, DECK_TEXT, DECK_LONGTEXT, false )
    add_string( "dj-theme", NULL, THEME_TEXT, THEME_LONGTEXT, true )

    add_submodule ()
        set_shortname( "DJ mix" )
        set_description( N_("The DJ's two decks, mixed, as a stream") )
        set_capability( "access_demux", 0 )
        add_shortcut( "djmix" )
        set_callbacks( DjMixOpen, DjMixClose )

    add_submodule ()
        set_shortname( "DJ deck" )
        set_description( N_("The end of a DJ deck's decoding chain") )
        set_capability( "sout stream", 0 )
        add_shortcut( "djdeck" )
        set_callbacks( DjDeckOpen, DjDeckClose )

    add_submodule ()
        set_shortname( "SoundCloud" )
        set_description( N_("SoundCloud tracks, resolved at play time") )
        set_capability( "access", 0 )
        add_shortcut( "soundcloud" )
        set_callbacks( ScOpen, NULL )
vlc_module_end ()

/*****************************************************************************
 * Types
 *****************************************************************************/
typedef struct
{
    const char *id, *title, *url, *genre, *tags, *desc, *now;
    int listeners;
    bool dynamic;   /* strdup'd fields, freed on refresh */
} station_t;

typedef struct
{
    char *url, *title, *artist;
    int64_t ms;     /* duration; 0 when unknown */
    bool endless;   /* a stream that never ends by itself: it rotates */
} item_t;

typedef struct
{
    int from;               /* minutes after midnight */
    char *name, *say;
    char **picks;
    size_t n_picks;
} pblock_t;

typedef struct { char *label, *target; } mood_t;

typedef struct route_t
{
    intf_sys_t *sys;
    const char *path;
    httpd_url_t *url;
} route_t;

#define CMDQ 16

struct intf_sys_t
{
    intf_thread_t  *intf;
    playlist_t     *playlist;
    httpd_host_t   *host;
    route_t        *routes;
    size_t          n_routes;
    char           *password, *logpath, *progpath;

    vlc_mutex_t     lock;           /* everything below but the queue and the caches */
    vlc_cond_t      wait;
    vlc_thread_t    thread;
    vlc_interrupt_t *interrupt;
    bool            started, quit, vars;

    station_t      *stations;
    size_t          n_stations;
    time_t          stations_at;

    pblock_t        *blocks;
    size_t          n_blocks;
    mood_t         *moods;
    size_t          n_moods;
    int             voice;          /* 0 off, 1 block changes, 2 everything */
    int             rotate, hold, per_pick;
    bool            air_at_start;

    bool            on_air, start_pending, busy, let_go, say_block;
    dj_mix_t       *mix;            /* the decks */
    dj_ui_t        *ui;             /* the deck window */
    input_item_t   *mix_item;       /* djmix://, the DJ's one item in the playlist */
    dj_track_t     *crate;          /* what plays next */
    size_t          n_crate, cap_crate;
    int             cur_block, cur_pick, fill_block, fill_pick, air_block;
    char          **own;            /* the DJ's own crate: dj-persona.json's "plays" */
    size_t          n_own;
    float           freedom;        /* the share of picks that are its own */
    time_t          own_at;         /* the file's time when read */
    int             set_from, set_len;   /* its own set, minutes from midnight, and long; -1 none */
    time_t          pick_started, hold_until, other_since, dry_until;
    int             dry_block;
    char           *said, *cur_label, *last_error, *live_url, *live_np, *say_next;

    vlc_mutex_t     qlock;          /* the queue from View > DJ, and the answers it reads */
    char           *cmdq[CMDQ];
    unsigned        cmdq_head, cmdq_n;
    char           *state_cache, *prog_cache;

    int             duck;
    char           *theme;
    dj_brain_t      brain;          /* what the deck window shows of the DJ; qlock */
};

/* the hand-picked stations; icecast over plain http so VLC's legacy HTTP access carries the
 * ICY now-playing tag (its HTTPS stack drops it) */
static const station_t builtin[] = {
 {"fip", "FIP", "http://icecast.radiofrance.fr/fip-hifi.aac", "eclectic jazz world electronic", "curated no-talk french paris anything", "Radio France's eclectic flagship: jazz, soul, world, electronic, rock, classical back to back, hardly a word spoken.", NULL, 0, false},
 {"fip-jazz", "FIP Jazz", "http://icecast.radiofrance.fr/fipjazz-hifi.aac", "jazz", "jazz bebop modal vocal", "FIP's all-jazz stream.", NULL, 0, false},
 {"fip-electro", "FIP Electro", "http://icecast.radiofrance.fr/fipelectro-hifi.aac", "electronic", "electro house techno downtempo", "FIP's electronic stream.", NULL, 0, false},
 {"fip-rock", "FIP Rock", "http://icecast.radiofrance.fr/fiprock-hifi.aac", "rock", "rock indie garage psych", "FIP's rock stream.", NULL, 0, false},
 {"fip-groove", "FIP Groove", "http://icecast.radiofrance.fr/fipgroove-hifi.aac", "funk soul", "groove funk soul disco", "FIP's funk, soul and groove stream.", NULL, 0, false},
 {"fip-world", "FIP Monde", "http://icecast.radiofrance.fr/fipworld-hifi.aac", "world", "world africa latin brazil asia", "FIP's world music stream.", NULL, 0, false},
 {"fip-reggae", "FIP Reggae", "http://icecast.radiofrance.fr/fipreggae-hifi.aac", "reggae", "reggae dub ska", "FIP's reggae stream.", NULL, 0, false},
 {"fip-new", "FIP Nouveautes", "http://icecast.radiofrance.fr/fipnouveautes-hifi.aac", "eclectic new", "new releases fresh", "FIP's new-releases stream.", NULL, 0, false},
 {"fip-pop", "FIP Pop", "http://icecast.radiofrance.fr/fippop-hifi.aac", "pop", "pop indie", "FIP's pop stream.", NULL, 0, false},
 {"fip-hiphop", "FIP Hip-Hop", "http://icecast.radiofrance.fr/fiphiphop-hifi.aac", "hiphop", "hip-hop rap beats", "FIP's hip-hop stream.", NULL, 0, false},
 {"fip-metal", "FIP Metal", "http://icecast.radiofrance.fr/fipmetal-hifi.aac", "metal", "metal heavy", "FIP's metal stream.", NULL, 0, false},
 {"rp", "Radio Paradise", "http://stream.radioparadise.com/flac", "eclectic rock", "listener-supported commercial-free classic rock indie world", "Radio Paradise main mix: eclectic, human-curated, commercial-free, lossless FLAC.", "https://api.radioparadise.com/api/now_playing?chan=0", 0, false},
 {"rp-mellow", "Radio Paradise Mellow", "http://stream.radioparadise.com/mellow-flac", "mellow acoustic", "mellow calm quiet acoustic evening focus", "Radio Paradise's mellow mix, lossless.", "https://api.radioparadise.com/api/now_playing?chan=1", 0, false},
 {"rp-rock", "Radio Paradise Rock", "http://stream.radioparadise.com/rock-flac", "rock", "rock classic alternative", "Radio Paradise's rock mix, lossless.", "https://api.radioparadise.com/api/now_playing?chan=2", 0, false},
 {"rp-global", "Radio Paradise Global", "http://stream.radioparadise.com/global-flac", "world", "world global latin african", "Radio Paradise's global mix, lossless.", "https://api.radioparadise.com/api/now_playing?chan=3", 0, false},
 {"rp-eclectic", "Radio Paradise Eclectic", "http://stream.radioparadise.com/eclectic-flac", "eclectic", "eclectic surprise", "Radio Paradise's eclectic mix.", NULL, 0, false},
 {"kexp", "KEXP", "http://kexp.streamguys1.com/kexp160.aac", "indie alternative", "seattle live djs indie rock new discovery", "KEXP Seattle: live DJs, indie and alternative, new music.", NULL, 0, false},
 {"nts1", "NTS 1", "http://stream-relay-geo.ntslive.net/stream", "eclectic underground", "london underground experimental electronic shows", "NTS Radio channel 1, London: curated shows, underground everything.", NULL, 0, false},
 {"nts2", "NTS 2", "http://stream-relay-geo.ntslive.net/stream2", "eclectic underground", "london underground experimental electronic shows", "NTS Radio channel 2.", NULL, 0, false},
 {"nightride", "Nightride FM", "http://stream.nightride.fm/nightride.mp3", "synthwave", "synthwave retrowave 80s neon night drive", "Synthwave and retrowave, around the clock.", NULL, 0, false},
 {"wfmu", "WFMU", "https://stream0.wfmu.org/freeform-128k", "freeform", "freeform weird jersey college eclectic", "WFMU Jersey City: freeform radio, anything goes.", NULL, 0, false},
 {"dublab", "dublab", "https://dublab.out.airtime.pro/dublab_a", "eclectic experimental", "los angeles experimental beats future", "dublab Los Angeles: future roots radio, beats and experiments.", NULL, 0, false},
 {"swiss-jazz", "Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128", "jazz", "jazz swing smooth no-talk", "Radio Swiss Jazz: jazz all day, no talk.", NULL, 0, false},
 {"swiss-classic", "Radio Swiss Classic", "http://stream.srg-ssr.ch/m/rsc_de/mp3_128", "classical", "classical orchestral piano baroque no-talk", "Radio Swiss Classic: classical music, no talk.", NULL, 0, false},
 {"record-techno", "Radio Record Techno", "http://radiorecord.hostingradio.ru/techno96.aacp", "techno", "techno peak-time energy club acid", "Radio Record's techno channel: driving, peak-time, acid passes through.", NULL, 0, false},
 {"record-minimal", "Radio Record Minimal", "http://radiorecord.hostingradio.ru/mini96.aacp", "minimal techno", "minimal tech micro groove focus office", "Radio Record's minimal/tech channel: stripped, hypnotic, works all day.", NULL, 0, false},
 {"record-deep", "Radio Record Deep", "http://radiorecord.hostingradio.ru/deep96.aacp", "deep house", "deep house warm late", "Radio Record's deep house channel.", NULL, 0, false},
 {"record-rap", "Radio Record Rap", "http://radiorecord.hostingradio.ru/rap96.aacp", "hiphop rap", "rap hip-hop hard trap", "Radio Record's rap channel: harder, newer.", NULL, 0, false},
 {"laut-techno", "laut.fm Techno", "https://stream.laut.fm/techno", "techno", "techno club berlin", "laut.fm's techno station.", "https://api.laut.fm/station/techno/current_song", 0, false},
 {"laut-minimal", "laut.fm Minimal", "https://stream.laut.fm/minimal", "minimal techno", "minimal tech focus", "laut.fm's minimal station.", "https://api.laut.fm/station/minimal/current_song", 0, false},
 {"laut-hiphop", "laut.fm Hip Hop", "https://stream.laut.fm/hiphop", "hiphop", "hip-hop rap", "laut.fm's hip hop station.", "https://api.laut.fm/station/hiphop/current_song", 0, false},
 {"laut-rap", "laut.fm Rap", "https://stream.laut.fm/rap", "hiphop rap", "rap hip-hop", "laut.fm's rap station.", "https://api.laut.fm/station/rap/current_song", 0, false},
 {"laut-oldschool", "laut.fm Oldschool Hip Hop", "https://stream.laut.fm/oldschool-hiphop", "hiphop oldschool", "old school boom bap golden era real hip-hop", "laut.fm's old school hip hop station: boom bap, golden era.", "https://api.laut.fm/station/oldschool-hiphop/current_song", 0, false},
 {"sunshine-techno", "Sunshine Live Techno", "https://stream.sunshine-live.de/techno/mp3-192/stream.sunshine-live.de/", "techno", "techno hard energy", "Sunshine Live's techno channel, Germany.", NULL, 0, false},
 {"rinse", "Rinse FM", "http://admin.stream.rinse.fm/proxy/rinse_uk/stream", "underground hiphop grime electronic", "london underground grime hip-hop bass club shows", "Rinse FM London: underground club music and hip hop, live shows.", NULL, 0, false},
 {"techno-fm", "Techno.FM", "http://stream.techno.fm/radio1-320k.mp3", "techno", "club techno shows slam radio peak-time 320", "Techno.FM London: club techno shows, Slam Radio among them, at 320 kbps.", NULL, 0, false},
 {"rp-serenity", "Radio Paradise Serenity", "http://stream.radioparadise.com/serenity-flac", "ambient", "ambient calm lossless", "Radio Paradise's ambient mix, lossless.", NULL, 0, false},
 {"wefunk", "WEFUNK Radio", "https://s-00.wefunkradio.com:8443/wefunk64.mp3", "funk hiphop", "funk soul real hip-hop boom bap montreal", "WEFUNK Montreal: funk, soul and real hip hop, since 1996.", NULL, 0, false},
};

/* written to dj-programme.json on first start, and used when that file does not read */
static const char default_programme[] =
"{\n"
" \"on_air_at_start\": true,\n"
" \"voice\": \"off\",\n"
" \"rotate_minutes\": 60,\n"
" \"hold_minutes\": 45,\n"
" \"tracks_per_pick\": 8,\n"
" \"blocks\": [\n"
"  {\n"
"   \"from\": \"07:00\",\n"
"   \"name\": \"Morning\",\n"
"   \"say\": \"Good morning. Techno.\",\n"
"   \"play\": [\n"
"    \"sc:@awakenings\",\n"
"    \"sc:@amelielens\",\n"
"    \"sc:@enricosangiuliano\",\n"
"    \"techno-fm\"\n"
"   ]\n"
"  },\n"
"  {\n"
"   \"from\": \"10:00\",\n"
"   \"name\": \"Office hours\",\n"
"   \"say\": \"Office hours. Club techno.\",\n"
"   \"play\": [\n"
"    \"sc:@adambeyer\",\n"
"    \"sc:@awakenings\",\n"
"    \"sc:@amelielens\",\n"
"    \"sc:@charlottedewittemusic\",\n"
"    \"techno-fm\"\n"
"   ]\n"
"  },\n"
"  {\n"
"   \"from\": \"13:00\",\n"
"   \"name\": \"Afternoon\",\n"
"   \"say\": \"Afternoon. Real hip hop.\",\n"
"   \"play\": [\n"
"    \"fip-hiphop\",\n"
"    \"wefunk\",\n"
"    \"sc:jonon\",\n"
"    \"laut-oldschool\",\n"
"    \"sc:cypress hill\",\n"
"    \"sc:korean hip hop\",\n"
"    \"sc:japanese hip hop\"\n"
"   ]\n"
"  },\n"
"  {\n"
"   \"from\": \"15:00\",\n"
"   \"name\": \"Late afternoon\",\n"
"   \"say\": \"Back to the club.\",\n"
"   \"play\": [\n"
"    \"sc:@adambeyer\",\n"
"    \"sc:@enricosangiuliano\",\n"
"    \"sc:@awakenings\",\n"
"    \"sc:acid techno\",\n"
"    \"techno-fm\"\n"
"   ]\n"
"  },\n"
"  {\n"
"   \"from\": \"18:00\",\n"
"   \"name\": \"Evening\",\n"
"   \"say\": \"Evening. Californication hours.\",\n"
"   \"play\": [\n"
"    \"sc:set californication soundtrack\",\n"
"    \"rp-rock\",\n"
"    \"kexp\",\n"
"    \"fip-rock\",\n"
"    \"wefunk\"\n"
"   ]\n"
"  },\n"
"  {\n"
"   \"from\": \"21:00\",\n"
"   \"name\": \"Night\",\n"
"   \"say\": \"Night. Peak time.\",\n"
"   \"play\": [\n"
"    \"sc:@daxj\",\n"
"    \"sc:@adambeyer\",\n"
"    \"sc:@paulatemple\",\n"
"    \"sc:@kobosil\",\n"
"    \"sc:acid techno\",\n"
"    \"techno-fm\"\n"
"   ]\n"
"  },\n"
"  {\n"
"   \"from\": \"01:00\",\n"
"   \"name\": \"Late\",\n"
"   \"say\": \"Late. Deeper.\",\n"
"   \"play\": [\n"
"    \"sc:@awakenings\",\n"
"    \"sc:deep techno\",\n"
"    \"record-deep\"\n"
"   ]\n"
"  }\n"
" ],\n"
" \"moods\": {\n"
"  \"Club techno\": \"sc:@adambeyer\",\n"
"  \"Awakenings\": \"sc:@awakenings\",\n"
"  \"Amelie Lens\": \"sc:@amelielens\",\n"
"  \"Hard techno\": \"sc:@daxj\",\n"
"  \"Acid techno\": \"sc:acid techno\",\n"
"  \"Techno radio\": \"techno-fm\",\n"
"  \"Minimal\": \"record-minimal\",\n"
"  \"Real hip hop\": \"wefunk\",\n"
"  \"Old school\": \"laut-oldschool\",\n"
"  \"Jonon\": \"sc:jonon\",\n"
"  \"Cypress Hill\": \"sc:cypress hill\",\n"
"  \"Korean hip hop\": \"sc:korean hip hop\",\n"
"  \"Japanese hip hop\": \"sc:japanese hip hop\",\n"
"  \"Californication\": \"sc:set californication soundtrack\",\n"
"  \"Rock, lossless\": \"rp-rock\",\n"
"  \"Underground\": \"rinse\",\n"
"  \"Deep\": \"record-deep\"\n"
" }\n"
"}\n";

#define SOMAFM_TTL (24 * 3600)
#define IA "https://archive.org"
#define SC_API "https://api-v2.soundcloud.com"
#define RB_API "https://de1.api.radio-browser.info/json/stations/search"

/*****************************************************************************
 * Small helpers: JSON out, query args, files, time
 *****************************************************************************/
static void jstr( struct vlc_memstream *ms, const char *s )
{
    if( s == NULL )
    {
        vlc_memstream_puts( ms, "null" );
        return;
    }
    vlc_memstream_putc( ms, '"' );
    for( const unsigned char *p = (const unsigned char *)s; *p; p++ )
    {
        switch( *p )
        {
            case '"':  vlc_memstream_puts( ms, "\\\"" ); break;
            case '\\': vlc_memstream_puts( ms, "\\\\" ); break;
            case '\n': vlc_memstream_puts( ms, "\\n" ); break;
            case '\r': vlc_memstream_puts( ms, "\\r" ); break;
            case '\t': vlc_memstream_puts( ms, "\\t" ); break;
            default:
                if( *p < 0x20 )
                    vlc_memstream_printf( ms, "\\u%04x", *p );
                else
                    vlc_memstream_putc( ms, *p );
        }
    }
    vlc_memstream_putc( ms, '"' );
}

static void jkv( struct vlc_memstream *ms, const char *key, const char *val, bool last )
{
    jstr( ms, key );
    vlc_memstream_putc( ms, ':' );
    jstr( ms, val );
    if( !last ) vlc_memstream_putc( ms, ',' );
}

static char *json_error( const char *fmt, ... )
{
    struct vlc_memstream ms;
    va_list ap;
    char *msg;
    va_start( ap, fmt );
    if( vasprintf( &msg, fmt, ap ) < 0 ) msg = NULL;
    va_end( ap );
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":false,\"error\":" );
    jstr( &ms, msg );
    vlc_memstream_puts( &ms, "}" );
    free( msg );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

/* one value of the query string, percent- and plus-decoded, malloc'd; NULL if absent */
static char *qarg( const uint8_t *args, const char *key )
{
    if( args == NULL ) return NULL;
    size_t klen = strlen( key );
    const char *p = (const char *)args;
    while( *p )
    {
        const char *end = strchr( p, '&' );
        size_t len = end ? (size_t)(end - p) : strlen( p );
        if( len > klen && !strncmp( p, key, klen ) && p[klen] == '=' )
        {
            char *v = strndup( p + klen + 1, len - klen - 1 );
            if( v == NULL ) return NULL;
            for( char *c = v; *c; c++ ) if( *c == '+' ) *c = ' ';
            vlc_uri_decode( v );
            return v;
        }
        if( end == NULL ) break;
        p = end + 1;
    }
    return NULL;
}

static char *lower_dup( const char *s )
{
    char *d = strdup( s ? s : "" );
    if( d ) for( char *c = d; *c; c++ ) *c = tolower( (unsigned char)*c );
    return d;
}

static char *capitalized( const char *s )
{
    char *d = strdup( s ? s : "" );
    if( d && d[0] ) d[0] = toupper( (unsigned char)d[0] );
    return d;
}

/* vlc_fopen("ab") does not append on Windows (vlc_open drops O_APPEND); the C runtime does */
static FILE *file_open( const char *path, const char *mode )
{
    if( path == NULL ) return NULL;
#ifdef _WIN32
    wchar_t *wpath = ToWide( path ), *wmode = ToWide( mode );
    FILE *f = wpath && wmode ? _wfopen( wpath, wmode ) : NULL;
    free( wpath ); free( wmode );
    return f;
#else
    return fopen( path, mode );
#endif
}

static char *read_file( const char *path )
{
    FILE *f = file_open( path, "rb" );
    if( f == NULL ) return NULL;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    char buf[4096];
    size_t got;
    while( (got = fread( buf, 1, sizeof buf, f )) > 0 )
        vlc_memstream_write( &ms, buf, got );
    fclose( f );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static void local_tm( time_t t, struct tm *tm )
{
#ifdef _WIN32
    localtime_s( tm, &t );
#else
    localtime_r( &t, tm );
#endif
}

static void hhmm( time_t t, char out[6] )
{
    struct tm tm;
    local_tm( t, &tm );
    snprintf( out, 6, "%02d:%02d", tm.tm_hour, tm.tm_min );
}

static void dj_log( intf_sys_t *sys, const char *verb, const char *what )
{
    FILE *f = file_open( sys->logpath, "ab" );
    if( f == NULL ) return;
    char ts[32];
    struct tm tm;
    local_tm( time( NULL ), &tm );
    strftime( ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_putc( &ms, '{' );
    jkv( &ms, "t", ts, false );
    jkv( &ms, "verb", verb, what == NULL );
    if( what ) jkv( &ms, "what", what, true );
    vlc_memstream_puts( &ms, "}\n" );
    if( !vlc_memstream_close( &ms ) )
    {
        fwrite( ms.ptr, 1, ms.length, f );
        free( ms.ptr );
    }
    fclose( f );
}

static int64_t jint( const json_value *o, const char *key )
{
    const json_value *v = o ? json_getbyname( o, key ) : NULL;
    if( v == NULL ) return 0;
    if( v->type == json_integer ) return v->u.integer;
    if( v->type == json_double ) return (int64_t)v->u.dbl;
    if( v->type == json_string ) return strtoll( v->u.string.ptr, NULL, 10 );
    return 0;
}

static int64_t jint_or( const json_value *o, const char *key, int64_t dflt )
{
    const json_value *v = o ? json_getbyname( o, key ) : NULL;
    return v && (v->type == json_integer || v->type == json_double) ? jint( o, key ) : dflt;
}

/*****************************************************************************
 * Fetching: VLC's own access, so http-proxy applies
 *****************************************************************************/
static int sleep_ms( int ms )
{
    return vlc_msleep_i11e( (mtime_t)ms * 1000 );
}

/* SoundCloud refuses about a third of calls from the tunnel's datacenter address with a bare
 * 403, at random: a refusal is retried on a fresh connection */
static char *fetch_text( vlc_object_t *obj, const char *url, int tries, size_t max )
{
    for( int i = 0; i < tries; i++ )
    {
        if( i > 0 && sleep_ms( 700 * i ) ) return NULL;   /* interrupted: stopping */
        int flags = obj->obj.flags;
        obj->obj.flags |= OBJECT_FLAGS_NOINTERACT;
        stream_t *s = vlc_stream_NewURL( obj, url );
        obj->obj.flags = flags;
        if( s == NULL ) continue;
        stream_t *chain = vlc_stream_FilterNew( s, "inflate" );
        if( chain ) s = chain;
        struct vlc_memstream ms;
        vlc_memstream_open( &ms );
        char buf[16384];
        ssize_t got;
        size_t total = 0;
        bool ok = true;
        while( (got = vlc_stream_Read( s, buf, sizeof buf )) > 0 )
        {
            vlc_memstream_write( &ms, buf, got );
            total += got;
            if( total > max ) { ok = false; break; }
        }
        vlc_stream_Delete( s );
        if( vlc_memstream_close( &ms ) ) continue;
        if( ok && total > 0 ) return ms.ptr;
        free( ms.ptr );
    }
    return NULL;
}

static json_value *fetch_json( vlc_object_t *obj, const char *url, int tries )
{
    char *text = fetch_text( obj, url, tries, 32 << 20 );
    if( text == NULL ) return NULL;
    json_settings settings;
    memset( &settings, 0, sizeof settings );
    char err[128];
    json_value *v = json_parse_ex( &settings, text, err );
    if( v == NULL ) msg_Warn( obj, "unreadable JSON from %s: %s", url, err );
    free( text );
    return v;
}

/*****************************************************************************
 * The playlist: volume, fades, what is on
 *****************************************************************************/
static int volume_get( intf_sys_t *sys )
{
    float v = playlist_VolumeGet( sys->playlist );
    return v < 0 ? -1 : (int)lroundf( v * 100.f );
}

static void volume_set( intf_sys_t *sys, int pct )
{
    if( pct < 0 ) pct = 0;
    if( pct > 125 ) pct = 125;
    playlist_VolumeSet( sys->playlist, pct / 100.f );
}

static int fade_to( intf_sys_t *sys, int target, float secs )
{
    int from = volume_get( sys );
    if( from < 0 ) return from;
    const int steps = 8;
    for( int i = 1; i <= steps; i++ )
    {
        volume_set( sys, from + (target - from) * i / steps );
        msleep( (mtime_t)(secs * CLOCK_FREQ / steps) );
    }
    return from;
}

/* the mix's own item in the playlist: djmix:// */
static input_item_t *mix_item( intf_sys_t *sys )
{
    if( sys->mix_item == NULL )
    {
        sys->mix_item = input_item_NewExt( DJ_MIX_URI, "DJ", -1, ITEM_TYPE_STREAM, ITEM_NET );
        if( sys->mix_item ) input_item_SetArtist( sys->mix_item, "DJ" );
    }
    return sys->mix_item;
}

enum { MIX_NONE, MIX_OPENING, MIX_PLAYING, MIX_PAUSED, MIX_OTHER };

/* what the playlist plays: the mix, something of the human's, or nothing */
static int mix_owner( intf_sys_t *sys )
{
    input_thread_t *in = playlist_CurrentInput( sys->playlist );
    if( in == NULL ) return MIX_NONE;
    int r = MIX_OTHER;
    if( sys->mix_item && input_GetItem( in ) == sys->mix_item )
        switch( var_GetInteger( in, "state" ) )
        {
            case PLAYING_S: r = MIX_PLAYING; break;
            case PAUSE_S:   r = MIX_PAUSED;  break;
            case INIT_S: case OPENING_S: r = MIX_OPENING; break;
            default:        r = MIX_NONE;    break;
        }
    vlc_object_release( in );
    return r;
}

/* the mix on: its item into the playlist when it is not there, and played */
static void mix_play( intf_sys_t *sys )
{
    input_item_t *it = mix_item( sys );
    if( it == NULL ) return;
    playlist_t *pl = sys->playlist;
    int own = mix_owner( sys );
    if( own == MIX_PAUSED ) { playlist_Resume( pl ); return; }
    if( own == MIX_PLAYING || own == MIX_OPENING ) return;
    playlist_Lock( pl );
    playlist_item_t *root = pl->p_playing, *found = NULL;
    for( int i = 0; root && !found && i < root->i_children; i++ )
        if( root->pp_children[i]->p_input == it ) found = root->pp_children[i];
    if( found ) playlist_ViewPlay( pl, root, found );
    playlist_Unlock( pl );
    if( !found ) playlist_AddInput( pl, it, true, true );
}

static bool playlist_empty( intf_sys_t *sys )
{
    playlist_Lock( sys->playlist );
    playlist_item_t *root = sys->playlist->p_playing;
    bool empty = root == NULL || root->i_children <= 0;
    playlist_Unlock( sys->playlist );
    return empty;
}

/*****************************************************************************
 * The voice
 *****************************************************************************/
#ifdef _WIN32
static const CLSID clsid_SpVoice = {0x96749377,0x3391,0x11D2,{0x9E,0xE3,0x00,0xC0,0x4F,0x79,0x73,0x96}};
static const IID   iid_ISpVoice  = {0x6C44DF74,0x72B9,0x4992,{0xA1,0xEC,0xEF,0x99,0x6E,0x04,0x22,0xD4}};

static bool speak( const char *text )
{
    wchar_t *w = ToWide( text );
    if( w == NULL ) return false;
    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    bool ok = false;
    ISpVoice *v = NULL;
    if( SUCCEEDED( CoCreateInstance( &clsid_SpVoice, NULL, CLSCTX_ALL, &iid_ISpVoice, (void **)&v ) ) )
    {
        ok = SUCCEEDED( ISpVoice_Speak( v, w, SPF_DEFAULT, NULL ) );
        ISpVoice_Release( v );
    }
    if( hr == S_OK || hr == S_FALSE ) CoUninitialize();
    free( w );
    return ok;
}
#else
static bool speak( const char *text ) { (void)text; return false; }
#endif

/* speak over the music, the mix ducked under the voice; never between tracks */
static bool announce( intf_sys_t *sys, const char *text, int duck )
{
    bool live = mix_owner( sys ) == MIX_PLAYING;
    if( live )
    {
        dj_mix_Duck( sys->mix, duck > 0 ? 20.f * log10f( duck / 100.f ) : -60.f );
        sleep_ms( 330 );
    }
    bool ok = speak( text );
    if( live ) dj_mix_Duck( sys->mix, 0.f );
    dj_log( sys, "say", text );
    return ok;
}

static void set_said( intf_sys_t *sys, const char *line )
{
    free( sys->said );
    sys->said = line ? strdup( line ) : NULL;
    if( line ) dj_log( sys, "said", line );
}

static void set_label( intf_sys_t *sys, const char *label )
{
    free( sys->cur_label );
    sys->cur_label = label ? strdup( label ) : NULL;
}

/*****************************************************************************
 * The catalogue: built-in plus SomaFM, fetched live
 *****************************************************************************/
static void stations_free( intf_sys_t *sys )
{
    for( size_t i = 0; i < sys->n_stations; i++ )
    {
        station_t *s = &sys->stations[i];
        if( !s->dynamic ) continue;
        free( (char *)s->id ); free( (char *)s->title ); free( (char *)s->url );
        free( (char *)s->genre ); free( (char *)s->tags ); free( (char *)s->desc );
        free( (char *)s->now );
    }
    free( sys->stations );
    sys->stations = NULL;
    sys->n_stations = 0;
}

static void stations_load( intf_sys_t *sys )
{
    time_t now = time( NULL );
    if( sys->stations && now - sys->stations_at < SOMAFM_TTL ) return;

    size_t n_builtin = sizeof builtin / sizeof builtin[0];
    station_t *list = NULL;
    size_t n = 0;

    json_value *root = fetch_json( VLC_OBJECT(sys->intf), "https://somafm.com/channels.json", 2 );
    const json_value *chans = root && root->type == json_object ? json_getbyname( root, "channels" ) : NULL;
    if( chans && chans->type == json_array )
    {
        list = calloc( n_builtin + chans->u.array.length, sizeof *list );
        for( unsigned i = 0; list && i < chans->u.array.length; i++ )
        {
            const json_value *c = chans->u.array.values[i];
            const char *id = jsongetstring( c, "id" );
            if( id == NULL ) continue;
            int br = 128;
            const json_value *pls = json_getbyname( c, "playlists" );
            if( pls && pls->type == json_array )
                for( unsigned k = 0; k < pls->u.array.length; k++ )
                {
                    const char *f = jsongetstring( pls->u.array.values[k], "format" );
                    const char *u = jsongetstring( pls->u.array.values[k], "url" );
                    if( f && u && !strcmp( f, "mp3" ) && strstr( u, "256" ) ) br = 256;
                }
            station_t *s = &list[n];
            char *url, *feed;
            if( asprintf( &url, "http://ice.somafm.com/%s-%d-mp3", id, br ) < 0 ) continue;
            if( asprintf( &feed, "https://somafm.com/songs/%s.json", id ) < 0 ) { free( url ); continue; }
            char *genre = json_dupstring( c, "genre" );
            if( genre ) for( char *g = genre; *g; g++ ) if( *g == '|' ) *g = ' ';
            s->id = strdup( id );
            s->title = json_dupstring( c, "title" );
            s->url = url;
            s->genre = genre ? genre : strdup( "" );
            s->tags = strdup( "somafm" );
            s->desc = json_dupstring( c, "description" );
            s->now = feed;
            s->listeners = (int)jint( c, "listeners" );
            s->dynamic = true;
            if( s->title == NULL ) s->title = strdup( id );
            if( s->desc == NULL ) s->desc = strdup( "" );
            n++;
        }
    }
    else
        msg_Warn( sys->intf, "SomaFM channel list unavailable; built-in stations only" );
    if( root ) json_value_free( root );

    if( list == NULL )
    {
        list = calloc( n_builtin, sizeof *list );
        if( list == NULL ) return;
        n = 0;
    }
    memcpy( list + n, builtin, n_builtin * sizeof *list );
    n += n_builtin;

    stations_free( sys );
    sys->stations = list;
    sys->n_stations = n;
    sys->stations_at = now;
}

static const station_t *station_of_uri( intf_sys_t *sys, const char *uri )
{
    if( uri == NULL ) return NULL;
    for( size_t i = 0; i < sys->n_stations; i++ )
    {
        const station_t *s = &sys->stations[i];
        if( !strcmp( s->url, uri ) ) return s;
        if( s->dynamic )   /* somafm: any ice*.somafm.com/<id>-... mirror */
        {
            char tag[128];
            snprintf( tag, sizeof tag, "somafm.com/%s-", s->id );
            if( strstr( uri, tag ) ) return s;
        }
    }
    return NULL;
}

static const station_t *station_find( intf_sys_t *sys, const char *key, char **err )
{
    stations_load( sys );
    char *k = lower_dup( key );
    const station_t *hit = NULL;
    size_t hits = 0;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    for( size_t i = 0; k && i < sys->n_stations; i++ )
    {
        const station_t *s = &sys->stations[i];
        if( !strcmp( s->id, k ) ) { hit = s; hits = 1; break; }
        char *t = lower_dup( s->title );
        bool m = strstr( s->id, k ) || (t && strstr( t, k ));
        free( t );
        if( m )
        {
            if( hits++ ) vlc_memstream_puts( &ms, ", " );
            vlc_memstream_puts( &ms, s->id );
            hit = s;
        }
    }
    free( k );
    bool ok = vlc_memstream_close( &ms ) == 0;
    if( hits == 1 )
    {
        if( ok ) free( ms.ptr );
        return hit;
    }
    *err = hits ? json_error( "'%s' names several stations: %s", key, ok ? ms.ptr : "" )
                : json_error( "no station '%s'; see /dj/stations?mood=<word>", key );
    if( ok ) free( ms.ptr );
    return NULL;
}

/* the first station whose id or title holds the words, for a person's request */
static const station_t *station_first( intf_sys_t *sys, const char *key )
{
    stations_load( sys );
    char *k = lower_dup( key );
    const station_t *hit = NULL;
    for( size_t i = 0; k && !hit && i < sys->n_stations; i++ )
        if( !strcmp( sys->stations[i].id, k ) ) hit = &sys->stations[i];
    for( size_t i = 0; k && !hit && i < sys->n_stations; i++ )
    {
        char *t = lower_dup( sys->stations[i].title );
        if( strstr( sys->stations[i].id, k ) || (t && strstr( t, k )) ) hit = &sys->stations[i];
        free( t );
    }
    free( k );
    return hit;
}

/*****************************************************************************
 * archive.org: search and the tracks of one item
 *****************************************************************************/
static char *ia_search_json( intf_sys_t *sys, const char *q, const char *collection, int rows )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    /* plain words: all of them, in the title or in the creator (full-text ranks junk by
     * downloads); a query with quotes, a colon or an operator is passed as written */
    if( strchr( q, '"' ) || strchr( q, ':' ) || strstr( q, " AND " ) || strstr( q, " OR " ) )
        vlc_memstream_printf( &ms, "(%s)", q );
    else
    {
        char *words = strdup( q ), *save = NULL;
        struct vlc_memstream anded;
        vlc_memstream_open( &anded );
        bool first = true;
        for( char *w = words ? strtok_r( words, " ", &save ) : NULL; w; w = strtok_r( NULL, " ", &save ) )
        {
            vlc_memstream_printf( &anded, "%s%s", first ? "" : " AND ", w );
            first = false;
        }
        free( words );
        if( vlc_memstream_close( &anded ) == 0 )
        {
            vlc_memstream_printf( &ms, "(title:(%s) OR creator:(%s))", anded.ptr, anded.ptr );
            free( anded.ptr );
        }
    }
    vlc_memstream_puts( &ms, " AND (mediatype:audio OR mediatype:etree)" );
    if( collection && *collection )
    {
        const char *c = !strcmp( collection, "live" ) ? "etree"
                      : !strcmp( collection, "78s" ) ? "georgeblood"
                      : !strcmp( collection, "music" ) ? "audio_music" : collection;
        vlc_memstream_printf( &ms, " AND collection:%s", c );
    }
    if( vlc_memstream_close( &ms ) ) return NULL;
    char *enc = vlc_uri_encode( ms.ptr );
    free( ms.ptr );
    if( enc == NULL ) return NULL;
    char *url;
    if( asprintf( &url, IA "/advancedsearch.php?q=%s&fl[]=identifier&fl[]=title&fl[]=creator"
                  "&fl[]=date&fl[]=downloads&sort[]=downloads+desc&rows=%d&output=json", enc, rows ) < 0 )
        url = NULL;
    free( enc );
    if( url == NULL ) return NULL;
    json_value *root = fetch_json( VLC_OBJECT(sys->intf), url, 2 );
    free( url );
    if( root == NULL || root->type != json_object )
    {
        if( root ) json_value_free( root );
        return json_error( "archive.org did not answer" );
    }
    const json_value *resp = json_getbyname( root, "response" );
    const json_value *docs = resp ? json_getbyname( resp, "docs" ) : NULL;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"items\":[" );
    if( docs && docs->type == json_array )
        for( unsigned i = 0; i < docs->u.array.length; i++ )
        {
            const json_value *d = docs->u.array.values[i];
            const json_value *cr = json_getbyname( d, "creator" );
            const char *creator = cr && cr->type == json_string ? cr->u.string.ptr
                                : cr && cr->type == json_array && cr->u.array.length
                                  && cr->u.array.values[0]->type == json_string ? cr->u.array.values[0]->u.string.ptr : NULL;
            if( i ) vlc_memstream_putc( &ms, ',' );
            vlc_memstream_putc( &ms, '{' );
            char *target;
            if( asprintf( &target, "ia:%s", jsongetstring( d, "identifier" ) ? jsongetstring( d, "identifier" ) : "" ) < 0 )
                target = NULL;
            jkv( &ms, "play", target, false );
            free( target );
            jkv( &ms, "id", jsongetstring( d, "identifier" ), false );
            jkv( &ms, "title", jsongetstring( d, "title" ), false );
            jkv( &ms, "creator", creator, false );
            jkv( &ms, "date", jsongetstring( d, "date" ), false );
            vlc_memstream_printf( &ms, "\"downloads\":%" PRId64 "}", jint( d, "downloads" ) );
        }
    vlc_memstream_puts( &ms, "]}" );
    json_value_free( root );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

typedef struct { char *name, *title, *length; int rank, track; } iafile_t;

static int iafile_cmp( const void *a, const void *b )
{
    const iafile_t *x = a, *y = b;
    bool xn = x->track < 0, yn = y->track < 0;
    if( xn != yn ) return xn ? 1 : -1;     /* numbered tracks first */
    if( !xn && x->track != y->track ) return x->track < y->track ? -1 : 1;
    return strcmp( x->name, y->name );
}

static int64_t length_ms( const char *s )
{
    if( s == NULL ) return 0;
    if( strchr( s, ':' ) )
    {
        int h = 0, m = 0, sec = 0;
        if( sscanf( s, "%d:%d:%d", &h, &m, &sec ) == 3 ) return ((int64_t)h * 3600 + m * 60 + sec) * 1000;
        if( sscanf( s, "%d:%d", &m, &sec ) == 2 ) return ((int64_t)m * 60 + sec) * 1000;
        return 0;
    }
    return (int64_t)(us_strtod( s, NULL ) * 1000.);
}

/* the audio files of one item, MP3 first, one per stem, in track order; count in *n */
static iafile_t *ia_tracks( intf_sys_t *sys, const char *ident, size_t *n, char **title )
{
    *n = 0;
    if( title ) *title = NULL;
    char *url;
    if( asprintf( &url, IA "/metadata/%s", ident ) < 0 ) return NULL;
    json_value *root = fetch_json( VLC_OBJECT(sys->intf), url, 2 );
    free( url );
    if( root == NULL ) return NULL;
    if( root->type != json_object ) { json_value_free( root ); return NULL; }
    const json_value *meta = json_getbyname( root, "metadata" );
    if( title && meta ) *title = json_dupstring( meta, "title" );
    const json_value *files = json_getbyname( root, "files" );
    iafile_t *out = NULL;
    size_t cap = 0;
    if( files && files->type == json_array )
        for( unsigned i = 0; i < files->u.array.length; i++ )
        {
            const json_value *f = files->u.array.values[i];
            const char *name = jsongetstring( f, "name" );
            const char *fmt = jsongetstring( f, "format" );
            if( name == NULL || fmt == NULL ) continue;
            /* lossless first: the tunnel carries FLAC with room to spare */
            int rank = !strcmp( fmt, "24bit Flac" ) ? 0 : !strcmp( fmt, "Flac" ) ? 1
                     : !strcmp( fmt, "VBR MP3" ) ? 2 : !strcmp( fmt, "MP3" ) ? 3
                     : !strcmp( fmt, "Ogg Vorbis" ) ? 4 : !strcmp( fmt, "128Kbps MP3" ) ? 5 : -1;
            if( rank < 0 ) continue;
            const char *dot = strrchr( name, '.' );
            size_t stem = dot ? (size_t)(dot - name) : strlen( name );
            size_t k;
            for( k = 0; k < *n; k++ )
            {
                const char *d2 = strrchr( out[k].name, '.' );
                size_t s2 = d2 ? (size_t)(d2 - out[k].name) : strlen( out[k].name );
                if( s2 == stem && !strncmp( out[k].name, name, stem ) ) break;
            }
            if( k < *n && out[k].rank <= rank ) continue;
            if( k == *n )
            {
                if( *n == cap )
                {
                    iafile_t *grown = realloc( out, (cap = cap ? cap * 2 : 32) * sizeof *out );
                    if( grown == NULL ) break;
                    out = grown;
                }
                memset( &out[k], 0, sizeof out[k] );
                (*n)++;
            }
            free( out[k].name ); free( out[k].title ); free( out[k].length );
            out[k].name = strdup( name );
            out[k].title = json_dupstring( f, "title" );
            out[k].length = json_dupstring( f, "length" );
            out[k].rank = rank;
            out[k].track = jint( f, "track" ) > 0 ? (int)jint( f, "track" ) : -1;
            if( out[k].title == NULL )
            {
                const char *base = strrchr( name, '/' );
                base = base ? base + 1 : name;
                const char *bd = strrchr( base, '.' );
                out[k].title = bd ? strndup( base, bd - base ) : strdup( base );
            }
        }
    json_value_free( root );
    if( *n ) qsort( out, *n, sizeof *out, iafile_cmp );
    return out;
}

static void ia_free( iafile_t *f, size_t n )
{
    for( size_t i = 0; i < n; i++ ) { free( f[i].name ); free( f[i].title ); free( f[i].length ); }
    free( f );
}

/*****************************************************************************
 * SoundCloud: the web player's client id, search, sets, a track's stream
 *****************************************************************************/
static vlc_mutex_t sc_lock = VLC_STATIC_MUTEX;
static char sc_cid[40];
static time_t sc_cid_at;

/* the client id sits in one of soundcloud.com's script assets, as {client_id:"..."} */
static bool sc_scrape( vlc_object_t *obj, char out[40] )
{
    char *html = fetch_text( obj, "https://soundcloud.com/", 6, 4 << 20 );
    if( html == NULL ) return false;
    char *srcs[48];
    int n = 0;
    for( char *p = html; n < 48 && (p = strstr( p, "<script" )) != NULL; )
    {
        char *end = strchr( p, '>' );
        if( end == NULL ) break;
        *end = '\0';
        char *src = strstr( p, " src=\"" );
        if( src )
        {
            src += 6;
            char *q = strchr( src, '"' );
            if( q )
            {
                *q = '\0';
                if( !strncmp( src, "https://", 8 ) ) srcs[n++] = src;
            }
        }
        p = end + 1;
    }
    bool found = false;
    for( int i = n - 1; i >= 0 && !found; i-- )   /* the newest assets carry it */
    {
        char *js = fetch_text( obj, srcs[i], 3, 16 << 20 );
        if( js == NULL ) continue;
        for( const char *m = strstr( js, "client_id:\"" ); m && !found; m = strstr( m, "client_id:\"" ) )
        {
            m += strlen( "client_id:\"" );
            size_t len = strspn( m, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" );
            if( len >= 20 && len < 40 && m[len] == '"' )
            {
                memcpy( out, m, len );
                out[len] = '\0';
                found = true;
            }
        }
        free( js );
    }
    free( html );
    return found;
}

static bool sc_client_id( vlc_object_t *obj, char out[40], bool refresh )
{
    vlc_mutex_lock( &sc_lock );
    bool cached = sc_cid[0] && !refresh && time( NULL ) - sc_cid_at < 12 * 3600;
    if( cached ) strcpy( out, sc_cid );
    vlc_mutex_unlock( &sc_lock );
    if( cached ) return true;
    char fresh[40];
    if( !sc_scrape( obj, fresh ) )
    {
        msg_Err( obj, "SoundCloud: no client id on soundcloud.com" );
        return false;
    }
    vlc_mutex_lock( &sc_lock );
    strcpy( sc_cid, fresh );
    sc_cid_at = time( NULL );
    vlc_mutex_unlock( &sc_lock );
    strcpy( out, fresh );
    return true;
}

/* one API call, the client id appended; a run of refusals scrapes a fresh id once */
static json_value *sc_call( vlc_object_t *obj, const char *path )
{
    for( int round = 0; round < 2; round++ )
    {
        char cid[40];
        if( !sc_client_id( obj, cid, round > 0 ) ) return NULL;
        char *url;
        bool full = !strncmp( path, "https://", 8 );
        if( asprintf( &url, "%s%s%cclient_id=%s", full ? "" : SC_API, path,
                      strchr( path, '?' ) ? '&' : '?', cid ) < 0 )
            return NULL;
        json_value *v = fetch_json( obj, url, 6 );
        free( url );
        if( v ) return v;
        if( vlc_killed() ) return NULL;
    }
    return NULL;
}

/* the stream to ask for: progressive if offered, else plain HLS, AAC first; NULL when only
 * previews or encrypted streams remain */
static const char *sc_transcoding( const json_value *track )
{
    const json_value *media = json_getbyname( track, "media" );
    const json_value *tcs = media ? json_getbyname( media, "transcodings" ) : NULL;
    if( tcs == NULL || tcs->type != json_array ) return NULL;
    const char *best = NULL;
    int best_rank = 99;
    for( unsigned i = 0; i < tcs->u.array.length; i++ )
    {
        const json_value *tc = tcs->u.array.values[i];
        const json_value *fmt = json_getbyname( tc, "format" );
        const char *proto = fmt ? jsongetstring( fmt, "protocol" ) : NULL;
        const char *mime = fmt ? jsongetstring( fmt, "mime_type" ) : NULL;
        const char *url = jsongetstring( tc, "url" );
        const json_value *snip = json_getbyname( tc, "snipped" );
        if( proto == NULL || mime == NULL || url == NULL ) continue;
        if( snip && snip->type == json_boolean && snip->u.boolean ) continue;
        int rank = !strcmp( proto, "progressive" ) ? 0
                 : strcmp( proto, "hls" ) ? 99
                 : strstr( mime, "mp4a" ) ? 1
                 : !strncmp( mime, "audio/mpeg", 10 ) && strcmp( mime, "audio/mpegurl" ) ? 2
                 : !strcmp( mime, "audio/mpegurl" ) ? 3 : 99;
        if( rank < best_rank ) { best_rank = rank; best = url; }
    }
    return best;
}

static bool sc_playable( const json_value *t )
{
    if( t == NULL || t->type != json_object ) return false;
    const char *policy = jsongetstring( t, "policy" );
    if( policy && strcmp( policy, "ALLOW" ) && strcmp( policy, "MONETIZE" ) ) return false;
    const json_value *st = json_getbyname( t, "streamable" );
    if( st && st->type == json_boolean && !st->u.boolean ) return false;
    if( jint( t, "duration" ) < 60000 ) return false;
    return sc_transcoding( t ) != NULL;
}

static void sc_item( const json_value *t, item_t *it )
{
    const json_value *user = json_getbyname( t, "user" );
    if( asprintf( &it->url, "soundcloud://tracks/%" PRId64, jint( t, "id" ) ) < 0 ) it->url = NULL;
    it->title = json_dupstring( t, "title" );
    it->artist = user ? json_dupstring( user, "username" ) : NULL;
    it->ms = jint( t, "duration" );
    it->endless = false;
}

/* a set's tracks in order; most come as bare ids and are fetched fifty at a time */
static item_t *sc_set_items( vlc_object_t *obj, const json_value *set, size_t max, size_t *n )
{
    *n = 0;
    const json_value *tracks = json_getbyname( set, "tracks" );
    if( tracks == NULL || tracks->type != json_array || tracks->u.array.length == 0 ) return NULL;
    unsigned total = tracks->u.array.length;
    const json_value **full = calloc( total, sizeof *full );
    json_value **batches = calloc( total / 50 + 2, sizeof *batches );
    item_t *items = calloc( total < max ? total : max, sizeof *items );
    size_t nb = 0;
    if( full == NULL || batches == NULL || items == NULL )
    {
        free( full ); free( batches ); free( items );
        return NULL;
    }
    unsigned stub[50];
    unsigned ns = 0;
    for( unsigned i = 0; i <= total; i++ )
    {
        if( i < total )
        {
            const json_value *t = tracks->u.array.values[i];
            if( json_getbyname( t, "title" ) ) { full[i] = t; continue; }
            stub[ns++] = i;
            if( ns < 50 ) continue;
        }
        if( ns == 0 ) continue;
        struct vlc_memstream ids;
        vlc_memstream_open( &ids );
        vlc_memstream_puts( &ids, "/tracks?ids=" );
        for( unsigned k = 0; k < ns; k++ )
            vlc_memstream_printf( &ids, "%s%" PRId64, k ? "%2C" : "", jint( tracks->u.array.values[stub[k]], "id" ) );
        if( vlc_memstream_close( &ids ) == 0 )
        {
            json_value *b = sc_call( obj, ids.ptr );
            free( ids.ptr );
            if( b && b->type == json_array )
            {
                batches[nb++] = b;
                for( unsigned r = 0; r < b->u.array.length; r++ )
                {
                    int64_t id = jint( b->u.array.values[r], "id" );
                    for( unsigned k = 0; k < ns; k++ )
                        if( jint( tracks->u.array.values[stub[k]], "id" ) == id )
                            full[stub[k]] = b->u.array.values[r];
                }
            }
            else if( b )
                json_value_free( b );
        }
        ns = 0;
        if( vlc_killed() ) break;
    }
    for( unsigned i = 0; i < total && *n < max; i++ )
        if( full[i] && sc_playable( full[i] ) )
            sc_item( full[i], &items[(*n)++] );
    for( size_t k = 0; k < nb; k++ ) json_value_free( batches[k] );
    free( batches );
    free( full );
    if( *n == 0 ) { free( items ); return NULL; }
    return items;
}

static void items_free( item_t *it, size_t n )
{
    for( size_t i = 0; it && i < n; i++ ) { free( it[i].url ); free( it[i].title ); free( it[i].artist ); }
    free( it );
}

/* a channel's full sets, twenty minutes or more: for a programme pick two at random of the
 * ten newest (a weekly show differs day to day), for a person the ten newest in order */
static item_t *sc_user_items( vlc_object_t *obj, const json_value *user, size_t max, size_t *n )
{
    *n = 0;
    char *path;
    if( asprintf( &path, "/users/%" PRId64 "/tracks?limit=50", jint( user, "id" ) ) < 0 ) return NULL;
    json_value *r = sc_call( obj, path );
    free( path );
    const json_value *coll = r && r->type == json_object ? json_getbyname( r, "collection" ) : NULL;
    const json_value *sets[10];
    size_t ns = 0;
    for( unsigned i = 0; coll && coll->type == json_array && i < coll->u.array.length && ns < 10; i++ )
    {
        const json_value *t = coll->u.array.values[i];
        if( sc_playable( t ) && jint( t, "duration" ) >= 20 * 60000 ) sets[ns++] = t;
    }
    bool pick = max <= 8;
    size_t want = pick && ns > 2 ? 2 : ns;
    item_t *items = want ? calloc( want, sizeof *items ) : NULL;
    if( items == NULL )
    {
        if( r ) json_value_free( r );
        return NULL;
    }
    for( size_t k = 0; k < want; k++ )
    {
        if( pick )
        {
            size_t j = k + (size_t)((unsigned long)vlc_mrand48() % (ns - k));
            const json_value *t = sets[j];
            sets[j] = sets[k];
            sets[k] = t;
        }
        sc_item( sets[k], &items[(*n)++] );
    }
    json_value_free( r );
    return items;
}

/* "set <words>": the best set of five tracks or more; "<words>": tracks; "@<name>" or a
 * soundcloud.com page: that channel's sets, that track, or that set */
static item_t *sc_items( intf_sys_t *sys, const char *spec, size_t max, size_t *n, char **err, char **label )
{
    vlc_object_t *obj = VLC_OBJECT(sys->intf);
    *n = 0;
    while( *spec == ' ' ) spec++;
    bool set = !strncmp( spec, "set ", 4 );
    if( set ) spec += 4;
    item_t *items = NULL;
    char *path = NULL;

    if( spec[0] == '@' || strstr( spec, "soundcloud.com/" ) )
    {
        char *page;
        if( spec[0] == '@' ? asprintf( &page, "https://soundcloud.com/%s", spec + 1 ) < 0
                           : asprintf( &page, "%s%s", strstr( spec, "://" ) ? "" : "https://", spec ) < 0 )
            return NULL;
        char *q = strpbrk( page, "?#" );
        if( q ) *q = '\0';
        char *enc = vlc_uri_encode( page );
        free( page );
        if( enc == NULL || asprintf( &path, "/resolve?url=%s", enc ) < 0 ) path = NULL;
        free( enc );
        json_value *r = path ? sc_call( obj, path ) : NULL;
        free( path );
        const char *kind = r && r->type == json_object ? jsongetstring( r, "kind" ) : NULL;
        if( kind && !strcmp( kind, "track" ) && sc_playable( r ) && (items = calloc( 1, sizeof *items )) )
        {
            sc_item( r, items );
            *n = 1;
            *label = items->title ? strdup( items->title ) : strdup( "a SoundCloud track" );
        }
        else if( kind && !strcmp( kind, "playlist" ) && (items = sc_set_items( obj, r, max, n )) )
        {
            if( asprintf( label, "%s, a SoundCloud set", jsongetstring( r, "title" ) ? jsongetstring( r, "title" ) : "?" ) < 0 )
                *label = NULL;
        }
        else if( kind && !strcmp( kind, "user" ) && (items = sc_user_items( obj, r, max, n )) )
        {
            const char *who = jsongetstring( r, "full_name" );
            if( who == NULL || !*who ) who = jsongetstring( r, "username" );
            if( asprintf( label, "%s, on SoundCloud", who ? who : spec ) < 0 ) *label = NULL;
        }
        if( r ) json_value_free( r );
        if( items == NULL ) *err = json_error( "SoundCloud has nothing playable at %s", spec );
        return items;
    }

    char *enc = vlc_uri_encode( spec );
    if( enc == NULL ) return NULL;
    if( set && asprintf( &path, "/search/playlists?q=%s&limit=10", enc ) >= 0 )
    {
        json_value *r = sc_call( obj, path );
        free( path );
        const json_value *coll = r && r->type == json_object ? json_getbyname( r, "collection" ) : NULL;
        for( unsigned i = 0; coll && coll->type == json_array && i < coll->u.array.length && !items; i++ )
        {
            const json_value *p = coll->u.array.values[i];
            if( jint( p, "track_count" ) < 5 ) continue;
            char *pp;
            if( asprintf( &pp, "/playlists/%" PRId64, jint( p, "id" ) ) < 0 ) continue;
            json_value *full = sc_call( obj, pp );
            free( pp );
            if( full && full->type == json_object && (items = sc_set_items( obj, full, max, n )) )
                if( asprintf( label, "%s, a SoundCloud set", jsongetstring( full, "title" ) ? jsongetstring( full, "title" ) : spec ) < 0 )
                    *label = NULL;
            if( full ) json_value_free( full );
        }
        if( r ) json_value_free( r );
    }
    if( items == NULL && asprintf( &path, "/search/tracks?q=%s&limit=40", enc ) >= 0 )
    {
        json_value *r = sc_call( obj, path );
        free( path );
        const json_value *coll = r && r->type == json_object ? json_getbyname( r, "collection" ) : NULL;
        size_t cap = max < 20 ? max : 20;
        if( coll && coll->type == json_array && (items = calloc( cap, sizeof *items )) )
            for( unsigned i = 0; i < coll->u.array.length && *n < cap; i++ )
                if( sc_playable( coll->u.array.values[i] ) )
                    sc_item( coll->u.array.values[i], &items[(*n)++] );
        if( r ) json_value_free( r );
        if( *n && asprintf( label, "%s, from SoundCloud", spec ) < 0 ) *label = NULL;
    }
    free( enc );
    if( *n == 0 )
    {
        free( items );
        *err = json_error( "SoundCloud has nothing playable for '%s'", spec );
        return NULL;
    }
    return items;
}

/* for the agent: tracks or sets for some words, each with a target to play */
static char *sc_search_json( intf_sys_t *sys, const char *q, bool sets, int rows )
{
    char *enc = vlc_uri_encode( q ), *path;
    if( enc == NULL ) return NULL;
    if( asprintf( &path, "/search/%s?q=%s&limit=%d", sets ? "playlists" : "tracks", enc, rows * 2 ) < 0 ) path = NULL;
    free( enc );
    json_value *r = path ? sc_call( VLC_OBJECT(sys->intf), path ) : NULL;
    free( path );
    const json_value *coll = r && r->type == json_object ? json_getbyname( r, "collection" ) : NULL;
    if( coll == NULL || coll->type != json_array )
    {
        if( r ) json_value_free( r );
        return json_error( "SoundCloud did not answer" );
    }
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"items\":[" );
    int shown = 0;
    for( unsigned i = 0; i < coll->u.array.length && shown < rows; i++ )
    {
        const json_value *t = coll->u.array.values[i];
        if( !sets && !sc_playable( t ) ) continue;
        const json_value *user = json_getbyname( t, "user" );
        if( shown++ ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_putc( &ms, '{' );
        jkv( &ms, "play", jsongetstring( t, "permalink_url" ), false );
        jkv( &ms, "title", jsongetstring( t, "title" ), false );
        jkv( &ms, "by", user ? jsongetstring( user, "username" ) : NULL, false );
        if( sets )
            vlc_memstream_printf( &ms, "\"tracks\":%" PRId64 "}", jint( t, "track_count" ) );
        else
            vlc_memstream_printf( &ms, "\"minutes\":%" PRId64 ",\"plays\":%" PRId64 "}",
                                  jint( t, "duration" ) / 60000, jint( t, "playback_count" ) );
    }
    vlc_memstream_puts( &ms, "]}" );
    json_value_free( r );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

/*****************************************************************************
 * radio-browser.info: the open directory of stations
 *****************************************************************************/
static json_value *rb_query( intf_sys_t *sys, const char *spec, int limit )
{
    while( *spec == ' ' ) spec++;
    const char *eq = strchr( spec, '=' );
    char *val = vlc_uri_encode( eq ? eq + 1 : spec ), *q = NULL, *url = NULL;
    if( val == NULL ) return NULL;
    int r = eq ? asprintf( &q, "%.*s=%s", (int)(eq - spec), spec, val ) : asprintf( &q, "tag=%s", val );
    free( val );
    if( r < 0 ) return NULL;
    r = asprintf( &url, RB_API "?%s&order=clickcount&reverse=true&hidebroken=true&limit=%d", q, limit );
    free( q );
    if( r < 0 ) return NULL;
    json_value *v = fetch_json( VLC_OBJECT(sys->intf), url, 3 );
    free( url );
    if( v && v->type != json_array ) { json_value_free( v ); v = NULL; }
    return v;
}

/* plain http when the station offers it: VLC's legacy access carries ICY titles */
static const char *rb_url( const json_value *s )
{
    const char *res = jsongetstring( s, "url_resolved" ), *raw = jsongetstring( s, "url" );
    const char *use = res && *res ? res : raw;
    if( use && !strncmp( use, "https://", 8 ) && raw && !strncmp( raw, "http://", 7 ) ) use = raw;
    return use;
}

static item_t *rb_items( intf_sys_t *sys, const char *spec, size_t *n, char **err, char **label )
{
    *n = 0;
    json_value *v = rb_query( sys, spec, 10 );
    item_t *it = NULL;
    for( unsigned i = 0; v && i < v->u.array.length && it == NULL; i++ )
    {
        const json_value *s = v->u.array.values[i];
        const char *url = rb_url( s );
        if( jint( s, "lastcheckok" ) != 1 || url == NULL ) continue;
        if( (it = calloc( 1, sizeof *it )) == NULL ) break;
        it->url = strdup( url );
        it->title = json_dupstring( s, "name" );
        it->endless = true;
        *n = 1;
        *label = strdup( it->title ? it->title : "a radio-browser station" );
    }
    if( v ) json_value_free( v );
    if( it == NULL ) *err = json_error( "radio-browser has no working station for '%s'", spec );
    return it;
}

static char *rb_search_json( intf_sys_t *sys, const char *spec, int rows )
{
    json_value *v = rb_query( sys, spec, rows );
    if( v == NULL ) return json_error( "radio-browser did not answer" );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"items\":[" );
    for( unsigned i = 0; i < v->u.array.length; i++ )
    {
        const json_value *s = v->u.array.values[i];
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_putc( &ms, '{' );
        jkv( &ms, "play", rb_url( s ), false );
        jkv( &ms, "name", jsongetstring( s, "name" ), false );
        jkv( &ms, "tags", jsongetstring( s, "tags" ), false );
        jkv( &ms, "country", jsongetstring( s, "countrycode" ), false );
        vlc_memstream_printf( &ms, "\"kbps\":%" PRId64 ",\"clicks\":%" PRId64 "}", jint( s, "bitrate" ), jint( s, "clickcount" ) );
    }
    vlc_memstream_puts( &ms, "]}" );
    json_value_free( v );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

/*****************************************************************************
 * Targets -> items
 *****************************************************************************/
static bool is_path( const char *t )
{
    return (isalpha( (unsigned char)t[0] ) && t[1] == ':' && (t[2] == '\\' || t[2] == '/'))
        || t[0] == '\\' || t[0] == '/';
}

static bool is_audio_file( const char *url )
{
    const char *dot = strrchr( url, '.' );
    if( dot == NULL || strchr( dot, '/' ) ) return false;
    static const char *const ext[] = { ".mp3", ".m4a", ".ogg", ".opus", ".flac", ".wav", ".aac" };
    for( size_t i = 0; i < ARRAY_SIZE(ext); i++ )
        if( !strncasecmp( dot, ext[i], strlen( ext[i] ) ) ) return true;
    return false;
}

/* a station id or a word of its title, sc:<words>, sc:set <words>, a soundcloud.com page,
 * rb:<tag> or rb:<field>=<value>, ia:<archive.org item>, a URL, a path */
static item_t *resolve( intf_sys_t *sys, const char *target, size_t max, size_t *n, char **err, char **label )
{
    *n = 0;
    *err = NULL;
    *label = NULL;
    if( !strncmp( target, "sc:", 3 ) ) return sc_items( sys, target + 3, max, n, err, label );
    if( strstr( target, "soundcloud.com/" ) ) return sc_items( sys, target, max, n, err, label );
    if( !strncmp( target, "rb:", 3 ) ) return rb_items( sys, target + 3, n, err, label );
    if( !strncmp( target, "ia:", 3 ) )
    {
        size_t nf;
        char *title = NULL;
        iafile_t *f = ia_tracks( sys, target + 3, &nf, &title );
        if( nf == 0 )
        {
            ia_free( f, nf );
            free( title );
            *err = json_error( "no audio in archive item %s", target + 3 );
            return NULL;
        }
        item_t *it = calloc( nf, sizeof *it );
        if( it == NULL ) { ia_free( f, nf ); free( title ); return NULL; }
        for( size_t i = 0; i < nf; i++ )
        {
            char *enc = vlc_uri_encode( f[i].name );
            if( enc == NULL || asprintf( &it[i].url, IA "/download/%s/%s", target + 3, enc ) < 0 )
                it[i].url = NULL;
            free( enc );
            /* vlc_uri_encode escapes '/', which archive paths may hold */
            if( it[i].url )
                for( char *p; (p = strstr( it[i].url + strlen( IA "/download/" ), "%2F" )); )
                    { *p = '/'; memmove( p + 1, p + 3, strlen( p + 3 ) + 1 ); }
            it[i].title = strdup( f[i].title );
            it[i].ms = length_ms( f[i].length );
        }
        ia_free( f, nf );
        *label = title ? title : strdup( target + 3 );
        *n = nf;
        return it;
    }
    item_t *it = calloc( 1, sizeof *it );
    if( it == NULL ) return NULL;
    if( strstr( target, "://" ) )
    {
        const char *base = strrchr( target, '/' );
        it->url = strdup( target );
        it->title = strdup( base && base[1] ? base + 1 : target );
        it->endless = !is_audio_file( target );
    }
    else if( is_path( target ) )
    {
        const char *base = strrchr( target, '\\' ) ? strrchr( target, '\\' ) : strrchr( target, '/' );
        char *native = strdup( target );   /* vlc_path2uri wants the platform's own separators */
        for( char *c = native; c && *c; c++ ) if( *c == '/' ) *c = DIR_SEP_CHAR;
        it->url = native ? vlc_path2uri( native, NULL ) : NULL;
        free( native );
        it->title = strdup( base && base[1] ? base + 1 : target );
    }
    else
    {
        const station_t *s = station_find( sys, target, err );
        if( s == NULL ) { free( it ); return NULL; }
        it->url = strdup( s->url );
        it->title = strdup( s->title );
        it->endless = true;
    }
    if( it->url == NULL )
    {
        items_free( it, 1 );
        *err = json_error( "cannot play '%s'", target );
        return NULL;
    }
    *label = strdup( it->title ? it->title : target );
    *n = 1;
    return it;
}

/* what a person typed: a mood, a station, a target, or else a SoundCloud search */
static char *request_target( intf_sys_t *sys, const char *text )
{
    while( *text == ' ' ) text++;
    for( size_t i = 0; i < sys->n_moods; i++ )
        if( !strcasecmp( sys->moods[i].label, text ) ) return strdup( sys->moods[i].target );
    if( !strncmp( text, "sc:", 3 ) || !strncmp( text, "rb:", 3 ) || !strncmp( text, "ia:", 3 )
     || strstr( text, "://" ) || strstr( text, "soundcloud.com/" ) || is_path( text ) )
        return strdup( text );
    const station_t *s = station_first( sys, text );
    if( s ) return strdup( s->id );
    char *t;
    return asprintf( &t, "sc:%s", text ) < 0 ? NULL : t;
}

/*****************************************************************************
 * The programme
 *****************************************************************************/
static int hhmm_parse( const char *s )
{
    int h, m;
    if( s && sscanf( s, "%d:%d", &h, &m ) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60 )
        return h * 60 + m;
    return -1;
}

static void programme_free( intf_sys_t *sys )
{
    for( size_t i = 0; i < sys->n_blocks; i++ )
    {
        pblock_t *b = &sys->blocks[i];
        free( b->name ); free( b->say );
        for( size_t k = 0; k < b->n_picks; k++ ) free( b->picks[k] );
        free( b->picks );
    }
    free( sys->blocks );
    sys->blocks = NULL;
    sys->n_blocks = 0;
    for( size_t i = 0; i < sys->n_moods; i++ ) { free( sys->moods[i].label ); free( sys->moods[i].target ); }
    free( sys->moods );
    sys->moods = NULL;
    sys->n_moods = 0;
}

static bool programme_parse( intf_sys_t *sys, const char *text )
{
    json_settings settings;
    memset( &settings, 0, sizeof settings );
    char err[128];
    json_value *root = json_parse_ex( &settings, text, err );
    if( root == NULL || root->type != json_object )
    {
        msg_Err( sys->intf, "programme: %s", root ? "not a JSON object" : err );
        if( root ) json_value_free( root );
        return false;
    }
    const json_value *blocks = json_getbyname( root, "blocks" );
    if( blocks == NULL || blocks->type != json_array || blocks->u.array.length == 0 )
    {
        msg_Err( sys->intf, "programme: no blocks" );
        json_value_free( root );
        return false;
    }
    pblock_t *bl = calloc( blocks->u.array.length, sizeof *bl );
    size_t nb = 0;
    for( unsigned i = 0; bl && i < blocks->u.array.length; i++ )
    {
        const json_value *b = blocks->u.array.values[i];
        int from = hhmm_parse( jsongetstring( b, "from" ) );
        const json_value *play = json_getbyname( b, "play" );
        if( from < 0 || play == NULL || play->type != json_array || play->u.array.length == 0 )
        {
            msg_Warn( sys->intf, "programme: block %u skipped: it needs \"from\": \"HH:MM\" and a \"play\" list", i );
            continue;
        }
        pblock_t *k = &bl[nb];
        k->picks = calloc( play->u.array.length, sizeof *k->picks );
        if( k->picks == NULL ) continue;
        for( unsigned j = 0; j < play->u.array.length; j++ )
            if( play->u.array.values[j]->type == json_string )
                k->picks[k->n_picks++] = strdup( play->u.array.values[j]->u.string.ptr );
        if( k->n_picks == 0 ) { free( k->picks ); k->picks = NULL; continue; }
        k->from = from;
        k->name = json_dupstring( b, "name" );
        if( k->name == NULL )
        {
            char t[8];
            snprintf( t, sizeof t, "%02d:%02d", from / 60, from % 60 );
            k->name = strdup( t );
        }
        k->say = json_dupstring( b, "say" );
        nb++;
    }
    if( nb == 0 )
    {
        free( bl );
        json_value_free( root );
        return false;
    }
    for( size_t i = 1; i < nb; i++ )   /* by the hour */
        for( size_t j = i; j > 0 && bl[j - 1].from > bl[j].from; j-- )
        {
            pblock_t t = bl[j]; bl[j] = bl[j - 1]; bl[j - 1] = t;
        }

    const json_value *moods = json_getbyname( root, "moods" );
    mood_t *md = NULL;
    size_t nm = 0;
    if( moods && moods->type == json_object && moods->u.object.length )
    {
        md = calloc( moods->u.object.length, sizeof *md );
        for( unsigned i = 0; md && i < moods->u.object.length; i++ )
        {
            const json_value *v = moods->u.object.values[i].value;
            if( v->type != json_string ) continue;
            md[nm].label = strdup( moods->u.object.values[i].name );
            md[nm].target = strdup( v->u.string.ptr );
            nm++;
        }
    }

    const char *voice = jsongetstring( root, "voice" );
    const json_value *air = json_getbyname( root, "on_air_at_start" );
    int rotate = (int)jint_or( root, "rotate_minutes", 60 );
    int hold = (int)jint_or( root, "hold_minutes", 45 );
    int per_pick = (int)jint_or( root, "tracks_per_pick", 8 );

    programme_free( sys );
    sys->blocks = bl;
    sys->n_blocks = nb;
    sys->moods = md;
    sys->n_moods = nm;
    sys->voice = voice == NULL ? 0 : !strcmp( voice, "all" ) ? 2 : !strcmp( voice, "blocks" ) ? 1 : 0;
    sys->rotate = rotate >= 0 ? rotate : 60;
    sys->hold = hold >= 0 ? hold : 45;
    sys->per_pick = per_pick > 0 ? per_pick : 8;
    sys->air_at_start = air == NULL || air->type != json_boolean || air->u.boolean;
    json_value_free( root );
    return true;
}

static const char *voice_name( int v )
{
    return v == 0 ? "off" : v == 2 ? "all" : "blocks";
}

static char *programme_json( intf_sys_t *sys )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true," );
    jkv( &ms, "path", sys->progpath, false );
    jkv( &ms, "voice", voice_name( sys->voice ), false );
    vlc_memstream_printf( &ms, "\"on_air_at_start\":%s,\"rotate_minutes\":%d,\"hold_minutes\":%d,"
                          "\"tracks_per_pick\":%d,\"blocks\":[",
                          sys->air_at_start ? "true" : "false", sys->rotate, sys->hold, sys->per_pick );
    for( size_t i = 0; i < sys->n_blocks; i++ )
    {
        const pblock_t *b = &sys->blocks[i];
        char from[8];
        snprintf( from, sizeof from, "%02d:%02d", b->from / 60, b->from % 60 );
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_putc( &ms, '{' );
        jkv( &ms, "from", from, false );
        jkv( &ms, "name", b->name, false );
        jkv( &ms, "say", b->say, false );
        vlc_memstream_puts( &ms, "\"play\":[" );
        for( size_t k = 0; k < b->n_picks; k++ )
        {
            if( k ) vlc_memstream_putc( &ms, ',' );
            jstr( &ms, b->picks[k] );
        }
        vlc_memstream_puts( &ms, "]}" );
    }
    vlc_memstream_puts( &ms, "],\"moods\":[" );
    for( size_t i = 0; i < sys->n_moods; i++ )
    {
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_putc( &ms, '{' );
        jkv( &ms, "label", sys->moods[i].label, false );
        jkv( &ms, "target", sys->moods[i].target, true );
        vlc_memstream_putc( &ms, '}' );
    }
    vlc_memstream_puts( &ms, "]}" );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static void cache_programme( intf_sys_t *sys )
{
    char *p = programme_json( sys );
    vlc_mutex_lock( &sys->qlock );
    free( sys->prog_cache );
    sys->prog_cache = p;
    vlc_mutex_unlock( &sys->qlock );
}

/* the file, or the default written there first; a file that does not read keeps what was */
static bool programme_load( intf_sys_t *sys )
{
    char *text = read_file( sys->progpath );
    if( text == NULL && sys->progpath )
    {
        FILE *f = file_open( sys->progpath, "wb" );
        if( f )
        {
            fwrite( default_programme, 1, sizeof default_programme - 1, f );
            fclose( f );
            msg_Info( sys->intf, "programme written: %s", sys->progpath );
        }
    }
    bool ok = text ? programme_parse( sys, text ) : programme_parse( sys, default_programme );
    if( !ok && sys->n_blocks == 0 )
    {
        msg_Err( sys->intf, "%s does not read as a programme; the built-in one is on", sys->progpath );
        programme_parse( sys, default_programme );
    }
    free( text );
    cache_programme( sys );
    return ok;
}

static int minutes_now( time_t now, int *sec )
{
    struct tm tm;
    local_tm( now, &tm );
    if( sec ) *sec = tm.tm_sec;
    return tm.tm_hour * 60 + tm.tm_min;
}

/* the block on air at this time: before the day's first block, last night's runs on */
static int block_at( intf_sys_t *sys, time_t now )
{
    if( sys->n_blocks == 0 ) return -1;
    int m = minutes_now( now, NULL );
    int b = (int)sys->n_blocks - 1;
    for( size_t i = 0; i < sys->n_blocks; i++ )
        if( sys->blocks[i].from <= m ) b = (int)i;
    return b;
}

static time_t next_boundary( intf_sys_t *sys, time_t now, int *which )
{
    int sec;
    int m = minutes_now( now, &sec );
    for( size_t i = 0; i < sys->n_blocks; i++ )
        if( sys->blocks[i].from > m )
        {
            if( which ) *which = (int)i;
            return now + (sys->blocks[i].from - m) * 60 - sec;
        }
    if( which ) *which = 0;
    return now + (24 * 60 - m + (sys->n_blocks ? sys->blocks[0].from : 0)) * 60 - sec;
}

static void set_hold( intf_sys_t *sys, time_t now )
{
    time_t until = sys->n_blocks ? next_boundary( sys, now, NULL ) : now;
    if( until < now + sys->hold * 60 ) until = now + sys->hold * 60;
    sys->hold_until = until;
}

/*****************************************************************************
 * The crate: the tracks the DJ plays next
 *****************************************************************************/
static void crate_clear( intf_sys_t *sys, bool keep_requests )
{
    size_t k = 0;
    for( size_t i = 0; i < sys->n_crate; i++ )
        if( keep_requests && sys->crate[i].requested ) sys->crate[k++] = sys->crate[i];
        else dj_track_Clean( &sys->crate[i] );
    sys->n_crate = k;
}

/* a pick's tracks: a request ahead of the DJ's own, behind the requests before it */
static void crate_add( intf_sys_t *sys, const item_t *items, size_t n, int block, int pick,
                       bool requested, const char *label )
{
    size_t k = 0;
    for( size_t i = 0; i < n; i++ ) if( items[i].url ) k++;
    if( sys->n_crate + k > sys->cap_crate )
    {
        size_t cap = sys->cap_crate ? sys->cap_crate : 64;
        while( cap < sys->n_crate + k ) cap *= 2;
        dj_track_t *g = realloc( sys->crate, cap * sizeof *g );
        if( g == NULL ) return;
        sys->crate = g;
        sys->cap_crate = cap;
    }
    size_t at = sys->n_crate;
    if( requested )
        for( at = 0; at < sys->n_crate && sys->crate[at].requested; at++ ) ;
    memmove( &sys->crate[at + k], &sys->crate[at], (sys->n_crate - at) * sizeof *sys->crate );
    for( size_t i = 0, j = at; i < n; i++ )
    {
        if( items[i].url == NULL ) continue;
        dj_track_t *t = &sys->crate[j++];
        memset( t, 0, sizeof *t );
        t->url = strdup( items[i].url );
        t->title = items[i].title ? strdup( items[i].title ) : NULL;
        t->artist = items[i].artist ? strdup( items[i].artist ) : NULL;
        t->label = label ? strdup( label ) : NULL;
        t->length_ms = items[i].ms;
        t->endless = items[i].endless;
        t->requested = requested;
        t->block = block;
        t->pick = pick;
    }
    sys->n_crate += k;
}

static bool crate_pop( intf_sys_t *sys, dj_track_t *out )
{
    if( sys->n_crate == 0 ) return false;
    *out = sys->crate[0];
    sys->n_crate--;
    memmove( &sys->crate[0], &sys->crate[1], sys->n_crate * sizeof *sys->crate );
    return true;
}

/*****************************************************************************
 * The autopilot
 *****************************************************************************/
/* the next pick of a block that yields music, from pick 'from' on */
static item_t *next_pick( intf_sys_t *sys, int block, int from, int *pick, size_t *n, char **label )
{
    const pblock_t *b = &sys->blocks[block];
    for( size_t k = 0; k < b->n_picks; k++ )
    {
        int p = (int)((from + k) % b->n_picks);
        char *err = NULL;
        item_t *items = resolve( sys, b->picks[p], sys->per_pick, n, &err, label );
        if( items )
        {
            free( err );
            *pick = p;
            return items;
        }
        msg_Warn( sys->intf, "pick %s: %s", b->picks[p], err ? err : "nothing" );
        dj_log( sys, "pick failed", b->picks[p] );
        free( err );
        if( vlc_killed() ) break;
    }
    return NULL;
}

static int random_pick( const pblock_t *b )
{
    return b->n_picks ? (int)((unsigned long)vlc_mrand48() % b->n_picks) : 0;
}

/* The DJ's own music: dj-persona.json, the agent's file, says what it likes to play and how much
 * of the time; the listener's programme is a preference, not a fence. Read again when it changes. */
static void own_load( intf_sys_t *sys )
{
    char *dir = config_GetUserDir( VLC_CONFIG_DIR ), *path = NULL;
    if( dir == NULL || asprintf( &path, "%s" DIR_SEP "dj-persona.json", dir ) < 0 ) path = NULL;
    free( dir );
    struct stat st;
    time_t at = path && vlc_stat( path, &st ) == 0 ? st.st_mtime : 0;
    if( at == sys->own_at )
    {
        free( path );
        return;
    }
    sys->own_at = at;
    for( size_t i = 0; i < sys->n_own; i++ ) free( sys->own[i] );
    free( sys->own );
    sys->own = NULL;
    sys->n_own = 0;
    sys->freedom = 0.f;
    sys->set_from = -1;
    sys->set_len = 0;
    char *text = at ? read_file( path ) : NULL;
    free( path );
    if( text == NULL ) return;
    json_settings settings;
    memset( &settings, 0, sizeof settings );
    char err[128];
    json_value *root = json_parse_ex( &settings, text, err );
    free( text );
    if( root == NULL ) return;
    const json_value *plays = root->type == json_object ? json_getbyname( root, "plays" ) : NULL;
    const json_value *fr = root->type == json_object ? json_getbyname( root, "freedom" ) : NULL;
    if( plays && plays->type == json_array && plays->u.array.length )
    {
        sys->own = calloc( plays->u.array.length, sizeof *sys->own );
        for( unsigned i = 0; sys->own && i < plays->u.array.length; i++ )
            if( plays->u.array.values[i]->type == json_string )
                sys->own[sys->n_own++] = strdup( plays->u.array.values[i]->u.string.ptr );
        sys->freedom = .3f;
    }
    if( fr && (fr->type == json_double || fr->type == json_integer) )
        sys->freedom = fr->type == json_double ? (float)fr->u.dbl : (float)fr->u.integer;
    const json_value *set = root->type == json_object ? json_getbyname( root, "set" ) : NULL;
    if( set && set->type == json_object && sys->n_own )
    {
        sys->set_from = hhmm_parse( jsongetstring( set, "from" ) );
        sys->set_len = (int)jint_or( set, "minutes", 60 );
        if( sys->set_len < 15 ) sys->set_len = 15;
        if( sys->set_len > 240 ) sys->set_len = 240;
    }
    if( !(sys->freedom >= 0.f) ) sys->freedom = 0.f;
    if( sys->freedom > 1.f ) sys->freedom = 1.f;
    json_value_free( root );
    char what[64];
    snprintf( what, sizeof what, "%zu of its own, %.0f %% of the time", sys->n_own, sys->freedom * 100. );
    dj_log( sys, "own crate", what );
}

/* now and then a pick of the DJ's own, as often as its freedom says; always, asked for its choice */
static bool own_set_now( const intf_sys_t *sys )
{
    if( sys->set_from < 0 ) return false;
    int sec, m = minutes_now( time( NULL ), &sec );
    return ((m - sys->set_from) % 1440 + 1440) % 1440 < sys->set_len;
}

static bool refill_own( intf_sys_t *sys, int block, bool always )
{
    own_load( sys );
    always = always || own_set_now( sys );   /* its own set: all its own */
    if( sys->n_own == 0 || (!always && (vlc_mrand48() & 0xffff) >= (long)(sys->freedom * 65536.f)) ) return false;
    const char *target = sys->own[(unsigned long)vlc_mrand48() % sys->n_own];
    size_t n;
    char *err = NULL, *label = NULL, *mine = NULL;
    item_t *items = resolve( sys, target, sys->per_pick, &n, &err, &label );
    if( items == NULL )
    {
        dj_log( sys, "own pick failed", target );
        free( err );
        free( label );
        return false;
    }
    if( asprintf( &mine, own_set_now( sys ) ? "%s, in the DJ's own set" : "%s, the DJ's own pick", label ? label : target ) < 0 ) mine = NULL;
    crate_add( sys, items, n, block, -2, false, mine ? mine : label );
    sys->fill_block = block;
    dj_log( sys, "own pick", target );
    free( mine );
    free( label );
    free( err );
    items_free( items, n );
    return true;
}

/* the block's next pick into the crate; a random one when the block is new */
static bool refill( intf_sys_t *sys, int block, bool fresh )
{
    if( block < 0 || block >= (int)sys->n_blocks ) return false;
    /* a block that gave nothing is asked again in a minute, not every second */
    time_t now = time( NULL );
    if( block == sys->dry_block && now < sys->dry_until ) return false;
    own_load( sys );
    if( (!fresh || own_set_now( sys )) && refill_own( sys, block, false ) ) return true;
    const pblock_t *b = &sys->blocks[block];
    int from = !fresh && sys->fill_block == block && sys->fill_pick >= 0 ? sys->fill_pick + 1
             : random_pick( b );
    int pick;
    size_t n;
    char *label = NULL;
    item_t *items = next_pick( sys, block, from, &pick, &n, &label );
    if( items == NULL )
    {
        dj_log( sys, "block has no music", b->name );
        sys->dry_block = block;
        sys->dry_until = now + 60;
        return false;
    }
    sys->dry_block = -1;
    crate_add( sys, items, n, block, pick, false, label );
    sys->fill_block = block;
    sys->fill_pick = pick;
    char what[300];
    snprintf( what, sizeof what, "%s: %zu track%s", label ? label : "?", n, n == 1 ? "" : "s" );
    dj_log( sys, "crate", what );
    free( label );
    items_free( items, n );
    return true;
}

/* the crate's first track into the free deck */
static bool cue_next( intf_sys_t *sys, int block )
{
    if( sys->n_crate == 0 && !refill( sys, block, false ) ) return false;
    dj_track_t t;
    if( !crate_pop( sys, &t ) ) return false;
    dj_mix_Cue( sys->mix, &t );
    dj_track_Clean( &t );
    return true;
}

/* a track came on: its pick is the DJ's now, the mix item says what plays, and the voice,
 * when it is on at all, speaks over the new track, never in a gap */
static void on_live( intf_sys_t *sys, const dj_deck_info_t *lv )
{
    const char *url = lv ? lv->t.url : NULL;
    bool same = url && sys->live_url ? !strcmp( url, sys->live_url ) : url == sys->live_url;
    if( same )
    {
        const char *np = lv ? lv->now_playing : NULL;
        if( np && *np && (!sys->live_np || strcmp( np, sys->live_np )) && sys->mix_item )
        {
            free( sys->live_np );
            sys->live_np = strdup( np );
            input_item_SetNowPlaying( sys->mix_item, np );
        }
        return;
    }
    free( sys->live_url );
    sys->live_url = url ? strdup( url ) : NULL;
    FREENULL( sys->live_np );
    if( lv == NULL ) return;
    time_t now = time( NULL );
    bool new_pick = !sys->cur_label || !lv->t.label || strcmp( sys->cur_label, lv->t.label );
    sys->cur_block = lv->t.block;
    sys->cur_pick = lv->t.pick;
    set_label( sys, lv->t.label );
    if( lv->t.requested && sys->hold_until < now ) set_hold( sys, now );
    if( sys->mix_item )
    {
        char *np = NULL;
        if( lv->t.artist && lv->t.title && asprintf( &np, "%s - %s", lv->t.artist, lv->t.title ) < 0 )
            np = NULL;
        input_item_SetNowPlaying( sys->mix_item, np ? np : lv->t.title );
        input_item_SetArtist( sys->mix_item, lv->t.artist ? lv->t.artist : "DJ" );
        input_item_SetAlbum( sys->mix_item, lv->t.label );
        free( np );
    }
    dj_log( sys, "on", lv->t.title );
    const pblock_t *b = lv->t.block >= 0 && lv->t.block < (int)sys->n_blocks ? &sys->blocks[lv->t.block] : NULL;
    char *line = NULL, *cap = capitalized( lv->t.label );
    if( sys->say_next )
    {
        line = sys->say_next;
        sys->say_next = NULL;
    }
    else if( sys->say_block && b && lv->t.block == sys->air_block && sys->voice >= 1 )
    {
        if( asprintf( &line, "%s %s.", b->say ? b->say : b->name, cap ? cap : "" ) < 0 ) line = NULL;
    }
    else if( sys->voice >= 2 && new_pick )
    {
        if( asprintf( &line, lv->t.requested ? "On request: %s." : "Now: %s.", cap ? cap : "" ) < 0 )
            line = NULL;
    }
    if( b && lv->t.block == sys->air_block ) sys->say_block = false;
    if( line )
    {
        announce( sys, line, sys->duck );
        set_said( sys, line );
    }
    free( line );
    free( cap );
}

/* on air: the block's music into the crate, the first track cued, the mix played */
static void start_air( intf_sys_t *sys, const char *why )
{
    time_t now = time( NULL );
    int block = sys->n_blocks ? block_at( sys, now ) : -1;
    sys->air_block = block;
    sys->hold_until = 0;
    crate_clear( sys, true );
    if( block >= 0 && sys->n_crate == 0 ) refill( sys, block, true );
    cue_next( sys, block );
    dj_mix_Next( sys->mix, DJ_NOW );
    sys->pick_started = now;
    sys->say_block = true;
    mix_play( sys );
    dj_log( sys, why, block >= 0 ? sys->blocks[block].name : NULL );
}

/* the DJ's second: whose music plays, the block, a station's hour, the next track cued */
static void tick( intf_sys_t *sys )
{
    if( !sys->on_air || sys->n_blocks == 0 ) return;
    time_t now = time( NULL );
    int own = mix_owner( sys );
    if( own == MIX_OTHER )
    {
        /* the human's choice plays: the decks wait, and let go after a minute */
        if( sys->other_since == 0 ) sys->other_since = now;
        else if( now - sys->other_since > 60 && !sys->let_go )
        {
            dj_mix_Clear( sys->mix );
            sys->let_go = true;
            dj_log( sys, "let go", "other media plays" );
        }
        return;
    }
    sys->other_since = 0;
    if( own == MIX_NONE )
    {
        if( sys->start_pending && playlist_empty( sys ) )
        {
            sys->start_pending = false;
            start_air( sys, "on air" );
        }
        return;   /* stopped by the human: the DJ waits */
    }
    sys->start_pending = false;
    if( own != MIX_PLAYING ) return;
    if( sys->let_go )
    {
        sys->let_go = false;
        start_air( sys, "back on air" );
        return;
    }

    dj_status_t st;
    dj_mix_Status( sys->mix, &st );
    const dj_deck_info_t *lv = st.live >= 0 ? &st.deck[st.live] : NULL;
    const dj_deck_info_t *cu = st.cue >= 0 ? &st.deck[st.cue] : NULL;
    on_live( sys, lv );
    int block = block_at( sys, now );
    bool held = now < sys->hold_until, busy = st.running || st.loading;
    if( block != sys->air_block && !held && !busy )
    {
        /* the block's music comes in soon, or with the end of a track nearly over */
        sys->air_block = block;
        crate_clear( sys, true );
        refill( sys, block, true );
        if( cu == NULL || !cu->t.requested ) cue_next( sys, block );
        double left = lv && lv->length_s > 0. ? lv->length_s - lv->pos_s : -1.;
        dj_mix_Next( sys->mix, left >= 0. && left < 360. ? DJ_NATURAL : DJ_SOON );
        sys->pick_started = now;
        sys->say_block = true;
        dj_log( sys, "block", sys->blocks[block].name );
    }
    else if( lv && lv->t.endless && !held && !busy && !st.armed && sys->rotate > 0
          && sys->blocks[block].n_picks > 1 && now - sys->pick_started >= sys->rotate * 60 )
    {
        /* a station has had its hour */
        if( cu == NULL ) cue_next( sys, block );
        dj_mix_Next( sys->mix, DJ_SOON );
        sys->pick_started = now;
        dj_log( sys, "rotate", sys->cur_label );
    }
    else if( cu == NULL && !busy
          && (lv == NULL || lv->eof || (lv->length_s > 0. && lv->length_s - lv->pos_s < 210.)) )
    {
        /* the next track, cued for the end of this one */
        if( cue_next( sys, block ) && lv == NULL ) dj_mix_Next( sys->mix, DJ_NOW );
    }
    dj_mix_StatusClean( &st );
}

/*****************************************************************************
 * State
 *****************************************************************************/
static const char *deck_state( int s )
{
    return s == DJ_LOADING ? "loading" : s == DJ_CUED ? "cued" : s == DJ_LIVE ? "live"
         : s == DJ_DONE ? "done" : s == DJ_FAILED ? "failed" : "empty";
}

static const char *mix_kind( int k )
{
    return k == DJ_BLEND ? "blend" : k == DJ_ECHO ? "echo" : k == DJ_FADE ? "fade"
         : k == DJ_START ? "start" : "none";
}

/* the source as the deck reads it; the mix itself is 48 kHz float */
static void audio_json( struct vlc_memstream *ms, const dj_deck_info_t *d )
{
    if( d == NULL || !d->codec[0] )
    {
        vlc_memstream_puts( ms, "\"audio\":null," );
        return;
    }
    vlc_memstream_puts( ms, "\"audio\":{" );
    jkv( ms, "codec", d->codec, false );
    vlc_memstream_printf( ms, "\"rate\":%u,", d->src_rate );
    if( d->src_bits ) vlc_memstream_printf( ms, "\"bits\":%u,", d->src_bits );
    vlc_memstream_printf( ms, "\"kbps\":%d,\"lossless\":%s,\"mix\":\"48 kHz float\"},",
                          d->kbps, d->lossless ? "true" : "false" );
}

static void deck_json( struct vlc_memstream *ms, int i, const dj_deck_info_t *d )
{
    vlc_memstream_printf( ms, "{\"deck\":\"%c\",", 'A' + i );
    jkv( ms, "state", deck_state( d->state ), d->state == DJ_EMPTY );
    if( d->state == DJ_EMPTY ) { vlc_memstream_putc( ms, '}' ); return; }
    jkv( ms, "title", d->t.title, false );
    jkv( ms, "artist", d->t.artist, false );
    jkv( ms, "pick", d->t.label, false );
    jkv( ms, "now_playing", d->now_playing, false );
    vlc_memstream_printf( ms, "\"bpm\":%.2f,\"on_grid\":%.2f,\"pitch\":%.2f,\"gain\":%.2f,"
                          "\"trim_db\":%.1f,\"bass_db\":%.1f,\"time\":%.0f,\"length\":%.0f,"
                          "\"ahead\":%.0f,\"lufs\":%.1f,\"requested\":%s}",
                          d->grid.period > 0. ? d->grid.bpm * d->rate : 0., d->grid.conf,
                          (d->rate - 1.) * 100., d->gain, d->trim_db, d->low_db, d->pos_s,
                          d->length_s, d->ahead_s, d->loud, d->t.requested ? "true" : "false" );
}

static char *state_json( intf_sys_t *sys )
{
    stations_load( sys );
    dj_status_t st;
    dj_mix_Status( sys->mix, &st );
    const dj_deck_info_t *lv = st.live >= 0 ? &st.deck[st.live] : NULL;
    const dj_deck_info_t *cu = st.cue >= 0 ? &st.deck[st.cue] : NULL;
    int own = mix_owner( sys );
    time_t now = time( NULL );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true," );

    const char *state = own == MIX_PLAYING ? "playing" : own == MIX_PAUSED ? "paused"
                      : own == MIX_OPENING ? "opening" : "stopped";
    char *uri = NULL, *title = NULL, *nowp = NULL;
    int64_t t = 0, len = 0;
    if( own == MIX_OTHER )
    {
        input_thread_t *in = playlist_CurrentInput( sys->playlist );
        if( in )
        {
            int s = var_GetInteger( in, "state" );
            state = s == PLAYING_S ? "playing" : s == PAUSE_S ? "paused" : "opening";
            input_item_t *it = input_GetItem( in );
            uri = input_item_GetURI( it );
            title = input_item_GetTitleFbName( it );
            nowp = input_item_GetNowPlayingFb( it );
            t = var_GetInteger( in, "time" ) / CLOCK_FREQ;
            len = var_GetInteger( in, "length" ) / CLOCK_FREQ;
            vlc_object_release( in );
        }
    }
    else if( lv )
    {
        uri = lv->t.url ? strdup( lv->t.url ) : NULL;
        title = lv->t.title ? strdup( lv->t.title ) : NULL;
        if( lv->now_playing ) nowp = strdup( lv->now_playing );
        else if( lv->t.artist && lv->t.title && asprintf( &nowp, "%s - %s", lv->t.artist, lv->t.title ) < 0 )
            nowp = NULL;
        t = (int64_t)lv->pos_s;
        len = (int64_t)lv->length_s;
    }
    const station_t *sn = station_of_uri( sys, uri );
    jkv( &ms, "state", state, false );
    jkv( &ms, "station", sn ? sn->id : NULL, false );
    jkv( &ms, "title", sn ? sn->title : title, false );
    jkv( &ms, "now_playing", nowp, false );
    jkv( &ms, "uri", uri, false );
    int count = (int)sys->n_crate + (cu ? 1 : 0);
    vlc_memstream_printf( &ms, "\"volume\":%d,\"time\":%" PRId64 ",\"length\":%" PRId64 ","
                          "\"queue\":{\"count\":%d,\"at\":0,\"left\":%d},",
                          volume_get( sys ), t, len, count, count );
    audio_json( &ms, own == MIX_OTHER ? NULL : lv );
    vlc_memstream_puts( &ms, "\"mix\":{" );
    jkv( &ms, "kind", mix_kind( st.kind ), false );
    vlc_memstream_printf( &ms, "\"armed\":%s,\"running\":%s,\"progress\":%.2f,\"starts_in\":%.0f,"
                          "\"beats\":%d,\"xfade\":%.2f,\"out_db\":%.1f,\"limiter_db\":%.1f,",
                          st.armed ? "true" : "false", st.running ? "true" : "false", st.progress,
                          st.starts_in_s, st.beats, st.xfade, st.out_db, st.gr_db );
    jkv( &ms, "plan", st.note[0] ? st.note : NULL, true );
    vlc_memstream_puts( &ms, "},\"decks\":[" );
    for( int i = 0; i < DJ_DECKS; i++ )
    {
        if( i ) vlc_memstream_putc( &ms, ',' );
        deck_json( &ms, i, &st.deck[i] );
    }
    vlc_memstream_puts( &ms, "]," );
    const char *next = cu ? cu->t.title : sys->n_crate ? sys->crate[0].title : NULL;
    jkv( &ms, "next", next, false );
    vlc_memstream_printf( &ms, "\"on_air\":%s,\"busy\":%s,", sys->on_air ? "true" : "false",
                          sys->busy || sys->cmdq_n ? "true" : "false" );
    jkv( &ms, "voice", voice_name( sys->voice ), false );
    jkv( &ms, "owner", own == MIX_OTHER ? "other" : own == MIX_NONE && !lv ? "none"
                     : lv && lv->t.requested ? "request" : "dj", false );
    if( sys->n_blocks )
    {
        int nb;
        char at_s[6];
        next_boundary( sys, now, &nb );
        hhmm( next_boundary( sys, now, NULL ), at_s );
        jkv( &ms, "block", sys->blocks[block_at( sys, now )].name, false );
        jkv( &ms, "next_block", sys->blocks[nb].name, false );
        jkv( &ms, "next_block_at", at_s, false );
    }
    if( now < sys->hold_until )
    {
        char held[6];
        hhmm( sys->hold_until, held );
        jkv( &ms, "held_until", held, false );
    }
    jkv( &ms, "pick", own == MIX_OTHER ? NULL : sys->cur_label, false );
    jkv( &ms, "error", sys->last_error, false );
    jkv( &ms, "said", sys->said, true );
    vlc_memstream_putc( &ms, '}' );
    free( uri ); free( title ); free( nowp );
    dj_mix_StatusClean( &st );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static void cache_state( intf_sys_t *sys )
{
    char *s = state_json( sys );
    if( s == NULL ) return;
    vlc_mutex_lock( &sys->qlock );
    free( sys->state_cache );
    sys->state_cache = s;
    vlc_mutex_unlock( &sys->qlock );
}

/*****************************************************************************
 * The verbs
 *****************************************************************************/
/* a person's choice: mixed in now, or next; it holds until the next block. The DJ takes
 * the playlist back for it, from whatever plays */
static char *play_target( intf_sys_t *sys, const char *target, bool now, const char *say )
{
    size_t n;
    char *err = NULL, *label = NULL;
    item_t *items = resolve( sys, target, 300, &n, &err, &label );
    if( items == NULL )
    {
        free( label );
        return err ? err : json_error( "out of memory" );
    }
    time_t t = time( NULL );
    int block = sys->n_blocks ? block_at( sys, t ) : -1;
    crate_add( sys, items, n, block, -1, true, label );
    dj_status_t st;
    dj_mix_Status( sys->mix, &st );
    bool cued_request = st.cue >= 0 && st.deck[st.cue].t.requested;
    dj_mix_StatusClean( &st );
    if( now || !cued_request ) cue_next( sys, block );
    if( now ) dj_mix_Next( sys->mix, DJ_NOW );
    set_hold( sys, t );
    free( sys->say_next );
    sys->say_next = say ? strdup( say ) : NULL;
    sys->let_go = false;
    mix_play( sys );
    char what[512];
    snprintf( what, sizeof what, "%s: %s (%zu item%s)", target, label ? label : "?", n, n == 1 ? "" : "s" );
    dj_log( sys, now ? "play" : "queue", what );
    items_free( items, n );
    free( label );
    return state_json( sys );
}

static char *do_play( intf_sys_t *sys, const uint8_t *args, bool now )
{
    char *target = qarg( args, "target" );
    if( target == NULL )
        return json_error( "target= is required: a station, sc:<words>, sc:set <words>, rb:<tag>, "
                           "ia:<item>, a URL or a path" );
    char *say = qarg( args, "say" );
    char *r = play_target( sys, target, now, say );
    free( target ); free( say );
    return r;
}

static char *do_request( intf_sys_t *sys, const uint8_t *args )
{
    char *text = qarg( args, "text" ), *queue = qarg( args, "queue" );
    if( text == NULL || !*text )
    {
        free( text ); free( queue );
        return json_error( "text= is required: a mood, a station, an artist, any words" );
    }
    char *target = request_target( sys, text );
    char *r = target ? play_target( sys, target, queue == NULL, NULL ) : json_error( "out of memory" );
    free( target ); free( text ); free( queue );
    return r;
}

static char *do_choose( intf_sys_t *sys )
{
    if( sys->n_blocks == 0 ) return json_error( "no programme" );
    int block = block_at( sys, time( NULL ) );
    sys->hold_until = 0;
    crate_clear( sys, false );
    if( !refill_own( sys, block, true ) && !refill( sys, block, true ) )
        return json_error( "nothing in %s plays right now", sys->blocks[block].name );
    cue_next( sys, block );
    dj_mix_Next( sys->mix, DJ_NOW );
    sys->air_block = block;
    sys->pick_started = time( NULL );
    sys->let_go = false;
    mix_play( sys );
    dj_log( sys, "dj's choice", NULL );
    return state_json( sys );
}

static char *do_air( intf_sys_t *sys, const uint8_t *args )
{
    char *on = qarg( args, "on" ), *voice = qarg( args, "voice" );
    if( voice )
        sys->voice = !strcmp( voice, "off" ) ? 0 : !strcmp( voice, "all" ) ? 2 : 1;
    if( on )
    {
        bool want = strcmp( on, "0" ) && strcmp( on, "off" ) && strcmp( on, "false" );
        sys->on_air = want;
        int own = mix_owner( sys );
        if( want && sys->n_blocks )
        {
            dj_status_t st;
            dj_mix_Status( sys->mix, &st );
            bool loaded = st.live >= 0 || st.cue >= 0 || st.loading;
            dj_mix_StatusClean( &st );
            sys->let_go = false;
            if( loaded ) mix_play( sys );
            else start_air( sys, "on air" );
        }
        else if( !want && own != MIX_OTHER )
        {
            if( own != MIX_NONE ) playlist_Stop( sys->playlist );
            dj_mix_Clear( sys->mix );
            crate_clear( sys, false );
        }
        dj_log( sys, want ? "on air" : "off air", NULL );
    }
    free( on ); free( voice );
    return state_json( sys );
}

static char *do_programme( intf_sys_t *sys, const uint8_t *args )
{
    char *reload = qarg( args, "reload" );
    bool ok = true;
    if( reload ) { ok = programme_load( sys ); dj_log( sys, "programme", ok ? "reloaded" : "kept: file does not read" ); }
    free( reload );
    if( !ok ) return json_error( "%s does not read as a programme; the one on air stays", sys->progpath );
    return programme_json( sys );
}

/* skip: the next track mixed in now; with the human's music on, VLC's own next */
static char *do_next( intf_sys_t *sys )
{
    if( mix_owner( sys ) == MIX_OTHER )
        playlist_Next( sys->playlist );
    else
    {
        dj_status_t st;
        dj_mix_Status( sys->mix, &st );
        bool cued = st.cue >= 0 || st.loading;
        dj_mix_StatusClean( &st );
        if( !cued ) cue_next( sys, sys->n_blocks ? block_at( sys, time( NULL ) ) : -1 );
        dj_mix_Next( sys->mix, DJ_NOW );
        mix_play( sys );
    }
    dj_log( sys, "skip", NULL );
    return state_json( sys );
}

static char *do_volume( intf_sys_t *sys, const uint8_t *args )
{
    char *level = qarg( args, "level" ), *secs = qarg( args, "secs" );
    if( level == NULL ) { free( secs ); return json_error( "level= is required: 0-125, +N or -N" ); }
    int cur = volume_get( sys );
    int want = (level[0] == '+' || level[0] == '-') ? cur + atoi( level ) : atoi( level );
    float f = secs ? (float)us_strtod( secs, NULL ) : 0.f;
    if( want < 0 ) want = 0;
    if( want > 125 ) want = 125;
    if( f > 0 ) fade_to( sys, want, f ); else volume_set( sys, want );
    free( level ); free( secs );
    char *out;   /* the playlist learns the new volume from the aout a moment later: answer the target */
    return asprintf( &out, "{\"ok\":true,\"volume\":%d,\"was\":%d}", want, cur ) < 0 ? NULL : out;
}

static char *do_say( intf_sys_t *sys, const uint8_t *args )
{
    char *text = qarg( args, "text" ), *duck = qarg( args, "duck" );
    if( text == NULL ) { free( duck ); return json_error( "text= is required" ); }
    bool ok = announce( sys, text, duck ? atoi( duck ) : sys->duck );
    set_said( sys, text );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_printf( &ms, "{\"ok\":%s,\"voice\":\"sapi\",\"said\":", ok ? "true" : "false" );
    jstr( &ms, text );
    vlc_memstream_putc( &ms, '}' );
    free( text ); free( duck );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static char *do_stations( intf_sys_t *sys, const uint8_t *args )
{
    stations_load( sys );
    char *mood = qarg( args, "mood" );
    char *m = lower_dup( mood );
    free( mood );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"stations\":[" );
    bool first = true;
    for( size_t i = 0; i < sys->n_stations; i++ )
    {
        const station_t *s = &sys->stations[i];
        if( m && *m )
        {
            char *hay;
            if( asprintf( &hay, "%s %s %s %s %s", s->id, s->title, s->genre, s->tags ? s->tags : "", s->desc ) < 0 ) continue;
            for( char *c = hay; *c; c++ ) *c = tolower( (unsigned char)*c );
            bool all = true;
            char *save = NULL, *words = strdup( m );
            for( char *w = words ? strtok_r( words, " ", &save ) : NULL; w; w = strtok_r( NULL, " ", &save ) )
                if( !strstr( hay, w ) ) { all = false; break; }
            free( words ); free( hay );
            if( !all ) continue;
        }
        if( !first ) vlc_memstream_putc( &ms, ',' );
        first = false;
        vlc_memstream_putc( &ms, '{' );
        jkv( &ms, "id", s->id, false );
        jkv( &ms, "title", s->title, false );
        jkv( &ms, "genre", s->genre, false );
        jkv( &ms, "description", s->desc, false );
        vlc_memstream_printf( &ms, "\"listeners\":%d}", s->listeners );
    }
    vlc_memstream_puts( &ms, "]}" );
    free( m );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static char *do_search( intf_sys_t *sys, const uint8_t *args )
{
    char *q = qarg( args, "q" ), *coll = qarg( args, "collection" ), *rows = qarg( args, "rows" ),
         *source = qarg( args, "source" );
    if( q == NULL ) { free( coll ); free( rows ); free( source ); return json_error( "q= is required" ); }
    int n = rows ? atoi( rows ) : 10;
    if( n <= 0 || n > 50 ) n = 10;
    char *out = !source || !strcmp( source, "archive" ) ? ia_search_json( sys, q, coll, n )
              : !strcmp( source, "soundcloud" ) ? sc_search_json( sys, q, false, n )
              : !strcmp( source, "sets" ) ? sc_search_json( sys, q, true, n )
              : !strcmp( source, "radio" ) ? rb_search_json( sys, q, n )
              : json_error( "source= is archive, soundcloud, sets or radio" );
    free( q ); free( coll ); free( rows ); free( source );
    return out;
}

static char *do_tracks( intf_sys_t *sys, const uint8_t *args )
{
    char *id = qarg( args, "id" );
    if( id == NULL ) return json_error( "id= is required: an archive.org item" );
    size_t n;
    char *title = NULL;
    iafile_t *f = ia_tracks( sys, id, &n, &title );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true," );
    jkv( &ms, "title", title, false );
    vlc_memstream_puts( &ms, "\"play\":" );
    char *tgt;
    if( asprintf( &tgt, "ia:%s", id ) >= 0 ) { jstr( &ms, tgt ); free( tgt ); } else jstr( &ms, NULL );
    vlc_memstream_puts( &ms, ",\"tracks\":[" );
    for( size_t i = 0; i < n; i++ )
    {
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_putc( &ms, '{' );
        jkv( &ms, "title", f[i].title, false );
        jkv( &ms, "length", f[i].length, true );
        vlc_memstream_putc( &ms, '}' );
    }
    vlc_memstream_puts( &ms, "]}" );
    ia_free( f, n );
    free( id ); free( title );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static char *do_playlist( intf_sys_t *sys )
{
    playlist_t *pl = sys->playlist;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"items\":[" );
    playlist_Lock( pl );
    playlist_item_t *root = pl->p_playing;
    playlist_item_t *cur = playlist_CurrentPlayingItem( pl );
    for( int i = 0; root && i < root->i_children; i++ )
    {
        playlist_item_t *it = root->pp_children[i];
        char *name = input_item_GetTitleFbName( it->p_input );
        char *artist = input_item_GetArtist( it->p_input );
        mtime_t d = input_item_GetDuration( it->p_input );
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_printf( &ms, "{\"id\":%d,\"current\":%s,\"duration\":%" PRId64 ",",
                              it->i_id, it == cur ? "true" : "false", d > 0 ? d / CLOCK_FREQ : (mtime_t)0 );
        jkv( &ms, "by", it->p_input == sys->mix_item ? "dj" : "other", false );
        jkv( &ms, "artist", artist, false );
        jkv( &ms, "name", name, true );
        vlc_memstream_putc( &ms, '}' );
        free( name ); free( artist );
    }
    playlist_Unlock( pl );
    vlc_memstream_puts( &ms, "]}" );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static char *do_remove( intf_sys_t *sys, const uint8_t *args )
{
    char *id = qarg( args, "id" );
    if( id == NULL ) return json_error( "id= is required (see /dj/playlist)" );
    playlist_t *pl = sys->playlist;
    playlist_Lock( pl );
    playlist_item_t *it = playlist_ItemGetById( pl, atoi( id ) );
    if( it ) playlist_NodeDelete( pl, it );
    playlist_Unlock( pl );
    bool found = it != NULL;
    free( id );
    if( !found ) return json_error( "no such item" );
    return state_json( sys );
}

static char *do_now( intf_sys_t *sys )
{
    char *st = state_json( sys );
    if( st == NULL ) return NULL;
    char *uri = NULL;
    dj_status_t ds;
    dj_mix_Status( sys->mix, &ds );
    if( ds.live >= 0 && ds.deck[ds.live].t.url ) uri = strdup( ds.deck[ds.live].t.url );
    dj_mix_StatusClean( &ds );
    const station_t *s = station_of_uri( sys, uri );
    free( uri );
    if( s == NULL || s->now == NULL ) return st;
    json_value *root = fetch_json( VLC_OBJECT(sys->intf), s->now, 2 );
    if( root == NULL || root->type != json_object )
    {
        if( root ) json_value_free( root );
        return st;
    }
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    st[strlen( st ) - 1] = '\0';   /* reopen the object */
    vlc_memstream_printf( &ms, "%s,\"recent\":[", st );
    free( st );
    const json_value *songs = json_getbyname( root, "songs" );
    if( songs && songs->type == json_array )
        for( unsigned i = 0; i < songs->u.array.length && i < 4; i++ )
        {
            const json_value *sg = songs->u.array.values[i];
            const char *a = jsongetstring( sg, "artist" ), *t = jsongetstring( sg, "title" ), *al = jsongetstring( sg, "album" );
            char *line;
            if( asprintf( &line, "%s - %s%s%s%s", a ? a : "?", t ? t : "?", al && *al ? " (" : "",
                          al && *al ? al : "", al && *al ? ")" : "" ) < 0 ) continue;
            if( i ) vlc_memstream_putc( &ms, ',' );
            jstr( &ms, line );
            free( line );
        }
    else if( json_getbyname( root, "artist" ) )
    {
        /* Radio Paradise: "artist" is a name; laut.fm: an object holding one */
        const json_value *a = json_getbyname( root, "artist" );
        const char *artist = a->type == json_object ? jsongetstring( a, "name" ) : jsongetstring( root, "artist" );
        char *line;
        if( asprintf( &line, "%s - %s", artist ? artist : "?",
                      jsongetstring( root, "title" ) ? jsongetstring( root, "title" ) : "?" ) >= 0 )
        {
            jstr( &ms, line );
            free( line );
        }
    }
    vlc_memstream_puts( &ms, "]}" );
    json_value_free( root );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static char *do_log( intf_sys_t *sys, const uint8_t *args )
{
    char *ns = qarg( args, "n" );
    int n = ns ? atoi( ns ) : 20;
    free( ns );
    if( n <= 0 || n > 1000 ) n = 20;
    FILE *f = file_open( sys->logpath, "rb" );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"lines\":[" );
    if( f )
    {
        char **ring = calloc( n, sizeof *ring );
        int head = 0, total = 0;
        char line[4096];
        while( ring && fgets( line, sizeof line, f ) )
        {
            size_t len = strlen( line );
            while( len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r') ) line[--len] = '\0';
            if( len == 0 || line[0] != '{' ) continue;
            free( ring[head] );
            ring[head] = strdup( line );
            head = (head + 1) % n;
            total++;
        }
        fclose( f );
        int count = total < n ? total : n;
        int start = total < n ? 0 : head;
        for( int i = 0; ring && i < count; i++ )
        {
            if( i ) vlc_memstream_putc( &ms, ',' );
            vlc_memstream_puts( &ms, ring[(start + i) % n] ? ring[(start + i) % n] : "null" );
        }
        if( ring ) for( int i = 0; i < n; i++ ) free( ring[i] );
        free( ring );
    }
    vlc_memstream_puts( &ms, "]}" );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

/* every verb; the caller holds sys->lock */
static char *dispatch_locked( intf_sys_t *sys, const char *verb, const uint8_t *args )
{
    if( !strcmp( verb, "state" ) )     return state_json( sys );
    if( !strcmp( verb, "now" ) )       return do_now( sys );
    if( !strcmp( verb, "play" ) )      return do_play( sys, args, true );
    if( !strcmp( verb, "queue" ) )     return do_play( sys, args, false );
    if( !strcmp( verb, "request" ) )   return do_request( sys, args );
    if( !strcmp( verb, "choose" ) )    return do_choose( sys );
    if( !strcmp( verb, "air" ) )       return do_air( sys, args );
    if( !strcmp( verb, "programme" ) ) return do_programme( sys, args );
    if( !strcmp( verb, "next" ) )      return do_next( sys );
    if( !strcmp( verb, "prev" ) )    { playlist_Prev( sys->playlist ); dj_log( sys, "prev", NULL ); sleep_ms( 500 ); return state_json( sys ); }
    if( !strcmp( verb, "pause" ) )   { playlist_TogglePause( sys->playlist ); sleep_ms( 250 ); return state_json( sys ); }
    if( !strcmp( verb, "stop" ) )    { playlist_Stop( sys->playlist ); dj_log( sys, "stop", NULL ); sleep_ms( 250 ); return state_json( sys ); }
    if( !strcmp( verb, "clear" ) )   { crate_clear( sys, false ); dj_mix_Clear( sys->mix ); sys->hold_until = 0; dj_log( sys, "clear", NULL ); return state_json( sys ); }
    if( !strcmp( verb, "deck" ) )      return do_deck( sys );
    if( !strcmp( verb, "remove" ) )    return do_remove( sys, args );
    if( !strcmp( verb, "volume" ) )    return do_volume( sys, args );
    if( !strcmp( verb, "say" ) )       return do_say( sys, args );
    if( !strcmp( verb, "stations" ) )  return do_stations( sys, args );
    if( !strcmp( verb, "search" ) )    return do_search( sys, args );
    if( !strcmp( verb, "tracks" ) )    return do_tracks( sys, args );
    if( !strcmp( verb, "playlist" ) )  return do_playlist( sys );
    if( !strcmp( verb, "log" ) )       return do_log( sys, args );
    return json_error( "no verb '%s'", verb );
}

static char *dispatch( intf_sys_t *sys, const char *verb, const uint8_t *args )
{
    vlc_mutex_lock( &sys->lock );
    char *body = dispatch_locked( sys, verb, args );
    cache_state( sys );
    vlc_mutex_unlock( &sys->lock );
    return body;
}

/*****************************************************************************
 * The doors: HTTP routes, and the playlist variable View > DJ uses
 *****************************************************************************/
static int Callback( httpd_callback_sys_t *p_sys, httpd_client_t *cl,
                     httpd_message_t *answer, const httpd_message_t *query )
{
    (void)cl;
    if( answer == NULL || query == NULL ) return VLC_SUCCESS;
    route_t *r = (route_t *)p_sys;
    char *body = dispatch( r->sys, r->path + strlen( "/dj/" ), query->psz_args );
    if( body == NULL ) body = strdup( "{\"ok\":false,\"error\":\"out of memory\"}" );

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 1;
    answer->i_type = HTTPD_MSG_ANSWER;
    answer->i_status = 200;
    httpd_MsgAdd( answer, "Content-Type", "application/json; charset=utf-8" );
    httpd_MsgAdd( answer, "Cache-Control", "no-cache" );
    answer->p_body = (uint8_t *)body;
    answer->i_body = body ? (int)strlen( body ) : 0;
    /* as httpd's own file callback does: without these an HTTP/1.1 client waits for more */
    if( httpd_MsgGet( query, "Connection" ) != NULL )
        httpd_MsgAdd( answer, "Connection", "close" );
    httpd_MsgAdd( answer, "Content-Length", "%d", answer->i_body );
    return VLC_SUCCESS;
}

static const char *const paths[] = {
    "/dj/state", "/dj/now", "/dj/play", "/dj/queue", "/dj/request", "/dj/choose", "/dj/air",
    "/dj/programme", "/dj/next", "/dj/prev", "/dj/pause", "/dj/stop", "/dj/clear", "/dj/remove",
    "/dj/volume", "/dj/say", "/dj/stations", "/dj/search", "/dj/tracks", "/dj/playlist", "/dj/log",
    "/dj/deck",
};

static bool cmd_push( intf_sys_t *sys, const char *cmd )
{
    char *c = strdup( cmd );
    if( c == NULL ) return false;
    vlc_mutex_lock( &sys->qlock );
    bool ok = sys->cmdq_n < CMDQ;
    if( ok ) sys->cmdq[(sys->cmdq_head + sys->cmdq_n++) % CMDQ] = c;
    vlc_mutex_unlock( &sys->qlock );
    if( !ok ) free( c );
    else vlc_cond_signal( &sys->wait );
    return ok;
}

static char *cmd_pop( intf_sys_t *sys )
{
    char *c = NULL;
    vlc_mutex_lock( &sys->qlock );
    if( sys->cmdq_n )
    {
        c = sys->cmdq[sys->cmdq_head];
        sys->cmdq_head = (sys->cmdq_head + 1) % CMDQ;
        sys->cmdq_n--;
    }
    vlc_mutex_unlock( &sys->qlock );
    return c;
}

static char *cached( intf_sys_t *sys, char *const *slot, const char *dflt )
{
    vlc_mutex_lock( &sys->qlock );
    char *s = strdup( *slot ? *slot : dflt );
    vlc_mutex_unlock( &sys->qlock );
    return s;
}

/* fresh when the DJ is free, else the last state it published */
static char *quick_state( intf_sys_t *sys )
{
    if( vlc_mutex_trylock( &sys->lock ) == 0 )
    {
        char *s = state_json( sys );
        vlc_mutex_unlock( &sys->lock );
        if( s ) return s;
    }
    return cached( sys, &sys->state_cache, "{\"ok\":true,\"state\":\"stopped\",\"busy\":true}" );
}

static int CommandCallback( vlc_object_t *obj, const char *var, vlc_value_t old,
                            vlc_value_t cur, void *data )
{
    VLC_UNUSED(obj); VLC_UNUSED(var); VLC_UNUSED(old);
    intf_sys_t *sys = data;
    const char *cmd = cur.psz_string ? cur.psz_string : "";
    char *reply;
    if( !strcmp( cmd, "state" ) )
        reply = quick_state( sys );
    else if( !strcmp( cmd, "programme" ) )
        reply = cached( sys, &sys->prog_cache, "{\"ok\":false,\"error\":\"no programme\"}" );
    else if( !strcmp( cmd, "deck" ) && vlc_mutex_trylock( &sys->lock ) == 0 )
    {   /* View > DJ wants the window now, not after whatever the DJ is busy with */
        reply = do_deck( sys );
        vlc_mutex_unlock( &sys->lock );
    }
    else if( cmd_push( sys, cmd ) )
        reply = strdup( "{\"ok\":true,\"queued\":true}" );
    else
        reply = strdup( "{\"ok\":false,\"error\":\"the DJ is busy; try again\"}" );
    var_SetString( sys->playlist, "dj-reply", reply ? reply : "{\"ok\":false}" );
    free( reply );
    return VLC_SUCCESS;
}

/* a verb from View > DJ, run on the DJ's thread; an error waits in state for the dialog */
static void run_command( intf_sys_t *sys, char *cmd )
{
    char *q = strchr( cmd, '?' );
    if( q ) *q++ = '\0';
    sys->busy = true;
    cache_state( sys );
    char *reply = dispatch_locked( sys, cmd, (const uint8_t *)q );
    sys->busy = false;
    FREENULL( sys->last_error );
    if( reply == NULL || strstr( reply, "\"ok\":false" ) == reply + 1 )
    {
        json_settings settings;
        memset( &settings, 0, sizeof settings );
        char err[128];
        json_value *v = reply ? json_parse_ex( &settings, reply, err ) : NULL;
        const char *e = v && v->type == json_object ? jsongetstring( v, "error" ) : NULL;
        sys->last_error = strdup( e ? e : "the DJ could not do that" );
        if( v ) json_value_free( v );
    }
    free( reply );
}

/*****************************************************************************
 * The deck window's view of the DJ
 *****************************************************************************/
static void cache_brain( intf_sys_t *sys )
{
    dj_brain_t b;
    memset( &b, 0, sizeof b );
    time_t now = time( NULL );
    b.on_air = sys->on_air;
    b.busy = sys->busy;
    b.volume = volume_get( sys );
    b.voice = sys->voice;
    if( sys->n_blocks )
    {
        int nb;
        time_t at = next_boundary( sys, now, &nb );
        snprintf( b.block, sizeof b.block, "%s", sys->blocks[block_at( sys, now )].name );
        snprintf( b.next_block, sizeof b.next_block, "%s", sys->blocks[nb].name );
        hhmm( at, b.next_at );
    }
    snprintf( b.pick, sizeof b.pick, "%s", sys->cur_label ? sys->cur_label : "" );
    snprintf( b.said, sizeof b.said, "%s", sys->said ? sys->said : "" );
    snprintf( b.error, sizeof b.error, "%s", sys->last_error ? sys->last_error : "" );
    for( size_t i = 0; i < sys->n_crate && b.n_next < 6; i++ )
    {
        const dj_track_t *t = &sys->crate[i];
        snprintf( b.next[b.n_next++], sizeof b.next[0], "%s%s%s", t->artist ? t->artist : "",
                  t->artist ? " - " : "", t->title ? t->title : "?" );
    }
    for( size_t i = 0; i < sys->n_moods && b.n_moods < 24; i++ )
        snprintf( b.moods[b.n_moods++], sizeof b.moods[0], "%s", sys->moods[i].label );
    vlc_mutex_lock( &sys->qlock );
    b.busy = b.busy || sys->cmdq_n;
    sys->brain = b;
    vlc_mutex_unlock( &sys->qlock );
}

static void BrainCb( void *opaque, dj_brain_t *out )
{
    intf_sys_t *sys = opaque;
    vlc_mutex_lock( &sys->qlock );
    *out = sys->brain;
    vlc_mutex_unlock( &sys->qlock );
}

static bool CommandCb( void *opaque, const char *cmd )
{
    return cmd_push( opaque, cmd );
}

static void MixLog( void *opaque, const char *verb, const char *what )
{
    dj_log( opaque, verb, what );
}

/* the deck window: shown, made first when VLC started without it */
static char *do_deck( intf_sys_t *sys )
{
    if( sys->ui == NULL && sys->mix )
        sys->ui = dj_ui_New( VLC_OBJECT(sys->intf), sys->mix, BrainCb, CommandCb, sys, sys->theme );
    if( sys->ui == NULL ) return json_error( "the deck window did not open" );
    dj_ui_Show( sys->ui );
    return state_json( sys );
}

static void *Run( void *data )
{
    intf_sys_t *sys = data;
    vlc_interrupt_set( sys->interrupt );
    mtime_t next_tick = mdate() + 4 * CLOCK_FREQ;   /* let the playlist settle first */
    vlc_mutex_lock( &sys->lock );
    while( !sys->quit )
    {
        mtime_t deadline = mdate() + CLOCK_FREQ / 2;
        if( deadline > next_tick ) deadline = next_tick;
        vlc_cond_timedwait( &sys->wait, &sys->lock, deadline );
        if( sys->quit ) break;
        char *cmd;
        while( !sys->quit && (cmd = cmd_pop( sys )) != NULL )
        {
            run_command( sys, cmd );
            free( cmd );
        }
        if( !sys->quit && mdate() >= next_tick )
        {
            tick( sys );
            next_tick = mdate() + CLOCK_FREQ;
        }
        cache_state( sys );
        cache_brain( sys );
    }
    vlc_mutex_unlock( &sys->lock );
    return NULL;
}

/*****************************************************************************
 * Open / Close
 *****************************************************************************/
static int Open( vlc_object_t *obj )
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = calloc( 1, sizeof *sys );
    if( sys == NULL ) return VLC_ENOMEM;
    intf->p_sys = sys;
    sys->intf = intf;
    sys->playlist = pl_Get( intf );
    sys->duck = var_InheritInteger( intf, "dj-duck" );
    sys->password = var_InheritString( intf, "http-password" );
    sys->cur_block = sys->cur_pick = sys->fill_block = sys->fill_pick = sys->air_block = sys->dry_block = -1;
    sys->set_from = -1;
    vlc_mutex_init( &sys->lock );
    vlc_mutex_init( &sys->qlock );
    vlc_cond_init( &sys->wait );

    char *dir = config_GetUserDir( VLC_CONFIG_DIR );
    if( dir == NULL || asprintf( &sys->logpath, "%s" DIR_SEP "dj.log", dir ) < 0 ) sys->logpath = NULL;
    sys->progpath = var_InheritString( intf, "dj-programme" );
    if( (sys->progpath == NULL || !*sys->progpath) && dir )
    {
        free( sys->progpath );
        if( asprintf( &sys->progpath, "%s" DIR_SEP "dj-programme.json", dir ) < 0 ) sys->progpath = NULL;
    }
    sys->theme = var_InheritString( intf, "dj-theme" );
    if( (sys->theme == NULL || !*sys->theme) && dir )
    {
        free( sys->theme );
        if( asprintf( &sys->theme, "%s" DIR_SEP "dj-theme.json", dir ) < 0 ) sys->theme = NULL;
    }
    free( dir );

    sys->mix = dj_mix_New( obj, MixLog, sys );
    if( sys->mix == NULL )
        msg_Err( intf, "the DJ's decks did not start" );
    vlc_mutex_lock( &sys->lock );
    programme_load( sys );
    sys->on_air = var_InheritBool( intf, "dj-on-air" );
    sys->start_pending = sys->on_air && sys->air_at_start;
    vlc_mutex_unlock( &sys->lock );

    /* the HTTP door shares VLC's server and its password; without one, only View > DJ talks */
    if( sys->password == NULL || !*sys->password )
        msg_Warn( intf, "no http-password: the DJ's HTTP routes are off; View > DJ still works" );
    else if( (sys->host = vlc_http_HostNew( obj )) == NULL )
        msg_Err( intf, "cannot start VLC's HTTP server (http-host, http-port); View > DJ still works" );
    else
    {
        sys->n_routes = ARRAY_SIZE(paths);
        sys->routes = calloc( sys->n_routes, sizeof *sys->routes );
        for( size_t i = 0; sys->routes && i < sys->n_routes; i++ )
        {
            route_t *r = &sys->routes[i];
            r->sys = sys;
            r->path = paths[i];
            r->url = httpd_UrlNew( sys->host, paths[i], "", sys->password );
            if( r->url ) httpd_UrlCatch( r->url, HTTPD_MSG_GET, Callback, (httpd_callback_sys_t *)r );
            else msg_Err( intf, "cannot register %s", paths[i] );
        }
    }

    var_Create( sys->playlist, "dj-command", VLC_VAR_STRING );
    var_Create( sys->playlist, "dj-reply", VLC_VAR_STRING );
    var_AddCallback( sys->playlist, "dj-command", CommandCallback, sys );
    sys->vars = true;

    if( sys->mix && var_InheritBool( intf, "dj-deck" ) )
    {
        sys->ui = dj_ui_New( obj, sys->mix, BrainCb, CommandCb, sys, sys->theme );
        dj_ui_Show( sys->ui );
    }

    sys->interrupt = vlc_interrupt_create();
    if( sys->interrupt && vlc_clone( &sys->thread, Run, sys, VLC_THREAD_PRIORITY_LOW ) == 0 )
        sys->started = true;
    else
        msg_Err( intf, "the DJ's thread did not start: requests only, no programme" );
    msg_Info( intf, "DJ %s: %zu blocks in %s", sys->on_air ? "on air" : "off air", sys->n_blocks,
              sys->progpath ? sys->progpath : "the built-in programme" );
    return VLC_SUCCESS;
}

static void Close( vlc_object_t *obj )
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;
    if( sys->ui ) dj_ui_Delete( sys->ui );
    if( sys->started )
    {
        vlc_interrupt_kill( sys->interrupt );   /* a fetch or a wait in progress gives up */
        vlc_mutex_lock( &sys->lock );
        sys->quit = true;
        vlc_cond_signal( &sys->wait );
        vlc_mutex_unlock( &sys->lock );
        vlc_join( sys->thread, NULL );
    }
    if( sys->interrupt ) vlc_interrupt_destroy( sys->interrupt );
    if( sys->vars )
    {
        var_DelCallback( sys->playlist, "dj-command", CommandCallback, sys );
        var_Destroy( sys->playlist, "dj-command" );
        var_Destroy( sys->playlist, "dj-reply" );
    }
    for( size_t i = 0; sys->routes && i < sys->n_routes; i++ )
        if( sys->routes[i].url ) httpd_UrlDelete( sys->routes[i].url );
    for( size_t i = 0; i < sys->n_own; i++ ) free( sys->own[i] );
    free( sys->own );
    free( sys->routes );
    if( sys->host ) httpd_HostDelete( sys->host );
    if( sys->mix ) dj_mix_Delete( sys->mix );
    crate_clear( sys, false );
    free( sys->crate );
    if( sys->mix_item ) input_item_Release( sys->mix_item );
    free( sys->live_url ); free( sys->live_np ); free( sys->say_next ); free( sys->theme );
    for( unsigned i = 0; i < sys->cmdq_n; i++ ) free( sys->cmdq[(sys->cmdq_head + i) % CMDQ] );
    programme_free( sys );
    stations_free( sys );
    vlc_cond_destroy( &sys->wait );
    vlc_mutex_destroy( &sys->qlock );
    vlc_mutex_destroy( &sys->lock );
    free( sys->state_cache ); free( sys->prog_cache );
    free( sys->said ); free( sys->cur_label ); free( sys->last_error );
    free( sys->password ); free( sys->logpath ); free( sys->progpath );
    free( sys );
}

/*****************************************************************************
 * soundcloud:// -- a track's stream, asked for when it plays
 *****************************************************************************/
static int ScOpen( vlc_object_t *obj )
{
    stream_t *access = (stream_t *)obj;
    const char *loc = access->psz_location;
    char *path = NULL;
    if( !strncmp( loc, "tracks/", 7 ) && isdigit( (unsigned char)loc[7] ) )
    {
        if( asprintf( &path, "/tracks/%" PRId64, (int64_t)strtoll( loc + 7, NULL, 10 ) ) < 0 ) path = NULL;
    }
    else if( !strncmp( loc, "resolve?url=", 12 ) )
    {
        if( asprintf( &path, "/resolve?url=%s", loc + 12 ) < 0 ) path = NULL;
    }
    else
    {
        msg_Err( access, "not a SoundCloud location: %s", loc );
        return VLC_EGENERIC;
    }
    if( path == NULL ) return VLC_ENOMEM;
    json_value *t = sc_call( obj, path );
    free( path );
    if( t == NULL || t->type != json_object )
    {
        msg_Err( access, "SoundCloud did not answer for %s", loc );
        if( t ) json_value_free( t );
        return VLC_EGENERIC;
    }
    int ret = VLC_EGENERIC;
    const char *kind = jsongetstring( t, "kind" );
    const char *tc = kind == NULL || !strcmp( kind, "track" ) ? sc_transcoding( t ) : NULL;
    if( tc == NULL )
        msg_Err( access, "SoundCloud offers no playable stream for %s: a preview, a set, or encrypted only", loc );
    else
    {
        json_value *s = sc_call( obj, tc );
        const char *url = s && s->type == json_object ? jsongetstring( s, "url" ) : NULL;
        char *u = url ? strdup( url ) : NULL;
        if( u )
        {
            if( access->p_input )   /* a track that came by its page gets its name */
            {
                input_item_t *item = input_GetItem( access->p_input );
                const json_value *user = json_getbyname( t, "user" );
                const char *title = jsongetstring( t, "title" );
                const char *artist = user ? jsongetstring( user, "username" ) : NULL;
                if( item && title ) input_item_SetTitle( item, title );
                if( item && artist ) input_item_SetArtist( item, artist );
            }
            msg_Dbg( access, "SoundCloud stream for %s", loc );
            access->psz_url = u;   /* the core keeps the old URL and frees it */
            ret = VLC_ACCESS_REDIRECT;
        }
        else
            msg_Err( access, "SoundCloud gave no stream for %s", loc );
        if( s ) json_value_free( s );
    }
    json_value_free( t );
    return ret;
}
