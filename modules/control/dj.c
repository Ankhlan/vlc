/*****************************************************************************
 * dj.c : the DJ deck, inside VLC
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
 * An "interface" module (--extraintf dj) that serves the DJ's verbs as JSON routes on
 * VLC's HTTP server (the same host and password as the lua http interface:
 * http-host, http-port, http-password):
 *
 *   /dj/state /dj/now /dj/playlist /dj/stations?mood= /dj/search?q=&collection=&rows=
 *   /dj/tracks?id= /dj/log?n=
 *   /dj/play?target=&say= /dj/queue?target= /dj/next /dj/prev /dj/pause /dj/stop /dj/clear
 *   /dj/remove?id= /dj/volume?level=&secs= /dj/say?text=&duck=
 *
 * A target is a station id or a word of its title, a URL, or ia:<archive.org item> for a
 * whole set. Stations: a built-in list plus SomaFM's channels fetched live (cached a day).
 * Every outside fetch is a vlc_stream, so it honours VLC's http-proxy.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_httpd.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_memstream.h>
#include <vlc_configuration.h>
#include <vlc_charset.h>
#include <vlc_fs.h>

#include <ctype.h>
#include <time.h>

#include "../misc/webservices/json.h"
#include "../misc/webservices/json_helper.h"

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

#define DUCK_TEXT N_("Volume while speaking")
#define DUCK_LONGTEXT N_("Percent of the current volume the music is ducked to while the DJ speaks.")
#define FADE_TEXT N_("Fade on switch")
#define FADE_LONGTEXT N_("Seconds of fade when a play switches what is on.")

vlc_module_begin ()
    set_shortname( "DJ" )
    set_description( N_("DJ deck: stations, sets and a voice on VLC's HTTP server") )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
    set_capability( "interface", 0 )
    set_callbacks( Open, Close )
    add_shortcut( "dj" )
    add_integer( "dj-duck", 25, DUCK_TEXT, DUCK_LONGTEXT, true )
    add_float( "dj-fade", 1.2, FADE_TEXT, FADE_LONGTEXT, true )
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
    char *url, *title, *station;
} item_t;

struct route_t;

struct intf_sys_t
{
    intf_thread_t  *intf;
    playlist_t     *playlist;
    httpd_host_t   *host;
    struct route_t *routes;
    size_t          n_routes;
    char           *password;
    char           *logpath;
    vlc_mutex_t     lock;

    station_t      *stations;
    size_t          n_stations;
    time_t          stations_at;

    int             duck;
    float           fade;
};

/* handed to httpd as its opaque httpd_callback_sys_t and cast back in Callback */
typedef struct route_t
{
    intf_sys_t *sys;
    const char *path;
    httpd_url_t *url;
} route_t;

/* the hand-picked stations; icecast over plain http so VLC's legacy HTTP access carries the
 * ICY now-playing tag (its HTTPS stack drops it) */
static const station_t builtin[] = {
 {"fip", "FIP", "http://icecast.radiofrance.fr/fip-midfi.mp3", "eclectic jazz world electronic", "curated no-talk french paris anything", "Radio France's eclectic flagship: jazz, soul, world, electronic, rock, classical back to back, hardly a word spoken.", NULL, 0, false},
 {"fip-jazz", "FIP Jazz", "http://icecast.radiofrance.fr/fipjazz-midfi.mp3", "jazz", "jazz bebop modal vocal", "FIP's all-jazz stream.", NULL, 0, false},
 {"fip-electro", "FIP Electro", "http://icecast.radiofrance.fr/fipelectro-midfi.mp3", "electronic", "electro house techno downtempo", "FIP's electronic stream.", NULL, 0, false},
 {"fip-rock", "FIP Rock", "http://icecast.radiofrance.fr/fiprock-midfi.mp3", "rock", "rock indie garage psych", "FIP's rock stream.", NULL, 0, false},
 {"fip-groove", "FIP Groove", "http://icecast.radiofrance.fr/fipgroove-midfi.mp3", "funk soul", "groove funk soul disco", "FIP's funk, soul and groove stream.", NULL, 0, false},
 {"fip-world", "FIP Monde", "http://icecast.radiofrance.fr/fipworld-midfi.mp3", "world", "world africa latin brazil asia", "FIP's world music stream.", NULL, 0, false},
 {"fip-reggae", "FIP Reggae", "http://icecast.radiofrance.fr/fipreggae-midfi.mp3", "reggae", "reggae dub ska", "FIP's reggae stream.", NULL, 0, false},
 {"fip-new", "FIP Nouveautes", "http://icecast.radiofrance.fr/fipnouveautes-midfi.mp3", "eclectic new", "new releases fresh", "FIP's new-releases stream.", NULL, 0, false},
 {"fip-pop", "FIP Pop", "http://icecast.radiofrance.fr/fippop-midfi.mp3", "pop", "pop indie", "FIP's pop stream.", NULL, 0, false},
 {"fip-hiphop", "FIP Hip-Hop", "http://icecast.radiofrance.fr/fiphiphop-midfi.mp3", "hiphop", "hip-hop rap beats", "FIP's hip-hop stream.", NULL, 0, false},
 {"fip-metal", "FIP Metal", "http://icecast.radiofrance.fr/fipmetal-midfi.mp3", "metal", "metal heavy", "FIP's metal stream.", NULL, 0, false},
 {"rp", "Radio Paradise", "http://stream.radioparadise.com/aac-320", "eclectic rock", "listener-supported commercial-free classic rock indie world", "Radio Paradise main mix: eclectic, human-curated, commercial-free.", "https://api.radioparadise.com/api/now_playing?chan=0", 0, false},
 {"rp-mellow", "Radio Paradise Mellow", "http://stream.radioparadise.com/mellow-320", "mellow acoustic", "mellow calm quiet acoustic evening focus", "Radio Paradise's mellow mix.", NULL, 0, false},
 {"rp-rock", "Radio Paradise Rock", "http://stream.radioparadise.com/rock-320", "rock", "rock classic alternative", "Radio Paradise's rock mix.", NULL, 0, false},
 {"rp-global", "Radio Paradise Global", "http://stream.radioparadise.com/global-320", "world", "world global latin african", "Radio Paradise's global mix.", NULL, 0, false},
 {"rp-eclectic", "Radio Paradise Eclectic", "http://stream.radioparadise.com/eclectic-320", "eclectic", "eclectic surprise", "Radio Paradise's eclectic mix.", NULL, 0, false},
 {"kexp", "KEXP", "http://kexp-mp3-128.streamguys1.com/kexp128.mp3", "indie alternative", "seattle live djs indie rock new discovery", "KEXP Seattle: live DJs, indie and alternative, new music.", NULL, 0, false},
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
 {"laut-techno", "laut.fm Techno", "https://stream.laut.fm/techno", "techno", "techno club berlin", "laut.fm's techno station.", NULL, 0, false},
 {"laut-minimal", "laut.fm Minimal", "https://stream.laut.fm/minimal", "minimal techno", "minimal tech focus", "laut.fm's minimal station.", NULL, 0, false},
 {"laut-hiphop", "laut.fm Hip Hop", "https://stream.laut.fm/hiphop", "hiphop", "hip-hop rap", "laut.fm's hip hop station.", NULL, 0, false},
 {"laut-rap", "laut.fm Rap", "https://stream.laut.fm/rap", "hiphop rap", "rap hip-hop", "laut.fm's rap station.", NULL, 0, false},
 {"laut-oldschool", "laut.fm Oldschool Hip Hop", "https://stream.laut.fm/oldschool-hiphop", "hiphop oldschool", "old school boom bap golden era real hip-hop", "laut.fm's old school hip hop station: boom bap, golden era.", NULL, 0, false},
 {"sunshine-techno", "Sunshine Live Techno", "https://stream.sunshine-live.de/techno/mp3-192/stream.sunshine-live.de/", "techno", "techno hard energy", "Sunshine Live's techno channel, Germany.", NULL, 0, false},
 {"rinse", "Rinse FM", "http://admin.stream.rinse.fm/proxy/rinse_uk/stream", "underground hiphop grime electronic", "london underground grime hip-hop bass club shows", "Rinse FM London: underground club music and hip hop, live shows.", NULL, 0, false},
 {"wefunk", "WEFUNK Radio", "https://s-00.wefunkradio.com:8443/wefunk64.mp3", "funk hiphop", "funk soul real hip-hop boom bap montreal", "WEFUNK Montreal: funk, soul and real hip hop, since 1996.", NULL, 0, false},
};

#define SOMAFM_TTL (24 * 3600)
#define IA "https://archive.org"

/*****************************************************************************
 * Small helpers: JSON out, query args, lowercase
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
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":false,\"error\":" );
    char *msg;
    va_start( ap, fmt );
    if( vasprintf( &msg, fmt, ap ) < 0 ) msg = NULL;
    va_end( ap );
    jstr( &ms, msg );
    free( msg );
    vlc_memstream_puts( &ms, "}" );
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

/* vlc_fopen("ab") truncated the log on every open here; the C runtime's own append does not */
static FILE *log_open( const char *path, const char *mode )
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

static void dj_log( intf_sys_t *sys, const char *verb, const char *what )
{
    FILE *f = log_open( sys->logpath, "ab" );
    if( f == NULL ) return;
    char ts[32];
    time_t now = time( NULL );
    struct tm tm;
#ifdef _WIN32
    localtime_s( &tm, &now );
#else
    localtime_r( &now, &tm );
#endif
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
        msleep( (vlc_tick_t)(secs * CLOCK_FREQ / steps) );
    }
    return from;
}

static input_state_e input_state( intf_sys_t *sys )
{
    input_thread_t *in = playlist_CurrentInput( sys->playlist );
    if( in == NULL ) return END_S;
    input_state_e st = var_GetInteger( in, "state" );
    vlc_object_release( in );
    return st;
}

static bool wait_playing( intf_sys_t *sys, float secs )
{
    for( int i = 0; i < (int)(secs * 10); i++ )
    {
        if( input_state( sys ) == PLAYING_S ) return true;
        msleep( CLOCK_FREQ / 10 );
    }
    return false;
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

static void stations_load( intf_sys_t *sys );

static char *state_json( intf_sys_t *sys )
{
    stations_load( sys );
    playlist_t *pl = sys->playlist;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );

    const char *state = "stopped";
    char *uri = NULL, *title = NULL, *nowp = NULL, *artist = NULL;
    int64_t t = 0, len = 0;
    input_thread_t *in = playlist_CurrentInput( pl );
    if( in )
    {
        switch( var_GetInteger( in, "state" ) )
        {
            case PLAYING_S: state = "playing"; break;
            case PAUSE_S:   state = "paused";  break;
            case OPENING_S: case INIT_S: state = "opening"; break;
            default: break;
        }
        input_item_t *item = input_GetItem( in );
        uri = input_item_GetURI( item );
        title = input_item_GetTitleFbName( item );
        nowp = input_item_GetNowPlayingFb( item );
        artist = input_item_GetArtist( item );
        t = var_GetInteger( in, "time" ) / CLOCK_FREQ;
        len = var_GetInteger( in, "length" ) / CLOCK_FREQ;
        vlc_object_release( in );
    }
    const station_t *st = station_of_uri( sys, uri );

    int count = 0, at = -1;
    playlist_Lock( pl );
    playlist_item_t *root = pl->p_playing;
    playlist_item_t *cur = playlist_CurrentPlayingItem( pl );
    if( root )
    {
        count = root->i_children;
        for( int i = 0; i < root->i_children; i++ )
            if( root->pp_children[i] == cur ) at = i;
    }
    playlist_Unlock( pl );

    vlc_memstream_puts( &ms, "{\"ok\":true," );
    jkv( &ms, "state", state, false );
    jkv( &ms, "station", st ? st->id : NULL, false );
    jkv( &ms, "title", st ? st->title : title, false );
    if( st || artist == NULL || nowp == NULL || strcmp( nowp, title ? title : "" ) )
        jkv( &ms, "now_playing", nowp, false );
    else
    {
        char *both;
        if( asprintf( &both, "%s - %s", artist, nowp ) >= 0 )
        {
            jkv( &ms, "now_playing", both, false );
            free( both );
        }
    }
    jkv( &ms, "uri", uri, false );
    vlc_memstream_printf( &ms, "\"volume\":%d,\"time\":%" PRId64 ",\"length\":%" PRId64 ",",
                          volume_get( sys ), t, len );
    vlc_memstream_printf( &ms, "\"queue\":{\"count\":%d,\"at\":%d,\"left\":%d}}",
                          count, at + 1, at < 0 ? count : count - at - 1 );
    free( uri ); free( title ); free( nowp ); free( artist );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

/*****************************************************************************
 * The catalogue: built-in plus SomaFM, fetched live through VLC's own HTTP access
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

    char *doc = json_retrieve_document( VLC_OBJECT(sys->intf), "https://somafm.com/channels.json" );
    json_value *root = doc ? json_parse_document( VLC_OBJECT(sys->intf), doc ) : NULL;
    const json_value *chans = root ? json_getbyname( root, "channels" ) : NULL;
    if( chans && chans->type == json_array )
    {
        list = calloc( n_builtin + chans->u.array.length, sizeof *list );
        if( list )
        {
            for( unsigned i = 0; i < chans->u.array.length; i++ )
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
                const json_value *lst = json_getbyname( c, "listeners" );
                s->id = strdup( id );
                s->title = json_dupstring( c, "title" );
                s->url = url;
                s->genre = genre ? genre : strdup( "" );
                s->tags = strdup( "somafm" );
                s->desc = json_dupstring( c, "description" );
                s->now = feed;
                s->listeners = lst && lst->type == json_string ? atoi( lst->u.string.ptr )
                             : lst && lst->type == json_integer ? (int)lst->u.integer : 0;
                s->dynamic = true;
                if( s->title == NULL ) s->title = strdup( id );
                if( s->desc == NULL ) s->desc = strdup( "" );
                n++;
            }
        }
    }
    else
        msg_Warn( sys->intf, "SomaFM channel list unavailable; built-in stations only" );
    if( root ) json_value_free( root );
    free( doc );

    if( list == NULL )
    {
        list = calloc( n_builtin, sizeof *list );
        if( list == NULL ) return;
    }
    memcpy( list + n, builtin, n_builtin * sizeof *list );
    n += n_builtin;

    stations_free( sys );
    sys->stations = list;
    sys->n_stations = n;
    sys->stations_at = now;
    msg_Dbg( sys->intf, "catalogue: %zu stations", n );
}

static const station_t *station_find( intf_sys_t *sys, const char *key, char **err )
{
    stations_load( sys );
    char *k = lower_dup( key );
    const station_t *hit = NULL;
    size_t hits = 0;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    for( size_t i = 0; i < sys->n_stations; i++ )
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
    char *doc = json_retrieve_document( VLC_OBJECT(sys->intf), url );
    free( url );
    json_value *root = doc ? json_parse_document( VLC_OBJECT(sys->intf), doc ) : NULL;
    free( doc );
    if( root == NULL ) return json_error( "archive.org did not answer" );
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
            const json_value *dl = json_getbyname( d, "downloads" );
            if( i ) vlc_memstream_putc( &ms, ',' );
            vlc_memstream_putc( &ms, '{' );
            jkv( &ms, "id", jsongetstring( d, "identifier" ), false );
            jkv( &ms, "title", jsongetstring( d, "title" ), false );
            jkv( &ms, "creator", creator, false );
            jkv( &ms, "date", jsongetstring( d, "date" ), false );
            vlc_memstream_printf( &ms, "\"downloads\":%" PRId64 "}",
                                  dl && dl->type == json_integer ? (int64_t)dl->u.integer : (int64_t)0 );
        }
    vlc_memstream_puts( &ms, "]}" );
    json_value_free( root );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

typedef struct { char *name, *title, *length; int rank, track; } iafile_t;

static int iafile_cmp( const void *a, const void *b )
{
    const iafile_t *x = a, *y = b;
    if( x->track != y->track )
        return (x->track < 0) - (y->track < 0) ? (x->track < 0) - (y->track < 0) : x->track - y->track;
    return strcmp( x->name, y->name );
}

/* the audio files of one item, MP3 first, one per stem, in track order; count in *n */
static iafile_t *ia_tracks( intf_sys_t *sys, const char *ident, size_t *n )
{
    *n = 0;
    char *url;
    if( asprintf( &url, IA "/metadata/%s", ident ) < 0 ) return NULL;
    char *doc = json_retrieve_document( VLC_OBJECT(sys->intf), url );
    free( url );
    json_value *root = doc ? json_parse_document( VLC_OBJECT(sys->intf), doc ) : NULL;
    free( doc );
    if( root == NULL ) return NULL;
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
            int rank = !strcmp( fmt, "VBR MP3" ) ? 0 : !strcmp( fmt, "MP3" ) ? 1
                     : !strcmp( fmt, "128Kbps MP3" ) ? 2 : !strcmp( fmt, "Ogg Vorbis" ) ? 3
                     : !strcmp( fmt, "Flac" ) ? 4 : -1;
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
            const char *tr = jsongetstring( f, "track" );
            const json_value *tn = json_getbyname( f, "track" );
            out[k].name = strdup( name );
            out[k].title = json_dupstring( f, "title" );
            out[k].length = json_dupstring( f, "length" );
            out[k].rank = rank;
            out[k].track = tr ? atoi( tr ) : tn && tn->type == json_integer ? (int)tn->u.integer : -1;
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
 * Targets -> items
 *****************************************************************************/
static void items_free( item_t *it, size_t n )
{
    for( size_t i = 0; i < n; i++ ) { free( it[i].url ); free( it[i].title ); free( it[i].station ); }
    free( it );
}

static item_t *resolve( intf_sys_t *sys, const char *target, size_t *n, char **err )
{
    *n = 0;
    *err = NULL;
    if( !strncmp( target, "ia:", 3 ) )
    {
        size_t nf;
        iafile_t *f = ia_tracks( sys, target + 3, &nf );
        if( nf == 0 )
        {
            ia_free( f, nf );
            *err = json_error( "no audio in archive item %s", target + 3 );
            return NULL;
        }
        item_t *it = calloc( nf, sizeof *it );
        if( it == NULL ) { ia_free( f, nf ); return NULL; }
        for( size_t i = 0; i < nf; i++ )
        {
            char *enc = vlc_uri_encode( f[i].name );
            if( enc == NULL || asprintf( &it[i].url, IA "/download/%s/%s", target + 3, enc ) < 0 )
                it[i].url = NULL;
            free( enc );
            /* vlc_uri_encode escapes '/', which archive paths may contain */
            if( it[i].url )
                for( char *p; (p = strstr( it[i].url + strlen( IA "/download/" ), "%2F" )); )
                    { *p = '/'; memmove( p + 1, p + 3, strlen( p + 3 ) + 1 ); }
            it[i].title = strdup( f[i].title );
        }
        ia_free( f, nf );
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
    }
    else
    {
        const station_t *s = station_find( sys, target, err );
        if( s == NULL ) { free( it ); return NULL; }
        it->url = strdup( s->url );
        it->title = strdup( s->title );
        it->station = strdup( s->id );
    }
    *n = 1;
    return it;
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

static bool announce( intf_sys_t *sys, const char *text, int duck )
{
    bool live = input_state( sys ) == PLAYING_S;
    int before = live ? fade_to( sys, duck, 0.6f ) : -1;
    bool ok = speak( text );
    if( live && before >= 0 ) fade_to( sys, before, 0.8f );
    dj_log( sys, "say", text );
    return ok;
}

/*****************************************************************************
 * The verbs
 *****************************************************************************/
static char *do_play( intf_sys_t *sys, const uint8_t *args, bool replace )
{
    char *target = qarg( args, "target" );
    if( target == NULL ) return json_error( "target= is required: a station, a URL, or ia:<item>" );
    size_t n;
    char *err;
    item_t *items = resolve( sys, target, &n, &err );
    if( items == NULL )
    {
        free( target );
        return err ? err : json_error( "out of memory" );
    }
    playlist_t *pl = sys->playlist;
    char *say = qarg( args, "say" );
    if( say )
    {
        announce( sys, say, sys->duck );
        free( say );
    }
    if( replace )
    {
        int vol = volume_get( sys );
        if( input_state( sys ) == PLAYING_S ) fade_to( sys, 0, 0.8f );
        playlist_Clear( pl, pl_Unlocked );
        for( size_t i = 0; i < n; i++ )   /* last arg: the playlist, not the media library */
            playlist_AddExt( pl, items[i].url, items[i].title, i == 0, 0, NULL, 0, true );
        if( vol > 0 )
        {
            volume_set( sys, 0 );
            if( wait_playing( sys, 12.f ) ) fade_to( sys, vol, sys->fade );
            else volume_set( sys, vol );
        }
    }
    else
    {
        for( size_t i = 0; i < n; i++ )
            playlist_AddExt( pl, items[i].url, items[i].title, false, 0, NULL, 0, true );
        if( input_state( sys ) == END_S ) playlist_Play( pl );
    }
    char what[256];
    snprintf( what, sizeof what, "%s (%zu item%s, first: %s)", target, n, n == 1 ? "" : "s", items[0].title );
    dj_log( sys, replace ? "play" : "queue", what );
    items_free( items, n );
    free( target );
    msleep( CLOCK_FREQ / 2 );
    return state_json( sys );
}

static char *do_volume( intf_sys_t *sys, const uint8_t *args )
{
    char *level = qarg( args, "level" );
    char *secs = qarg( args, "secs" );
    if( level == NULL ) { free( secs ); return json_error( "level= is required: 0-125, +N or -N" ); }
    int cur = volume_get( sys );
    int want = (level[0] == '+' || level[0] == '-') ? cur + atoi( level ) : atoi( level );
    float f = secs ? (float)atof( secs ) : 0.f;
    if( f > 0 ) fade_to( sys, want, f ); else volume_set( sys, want );
    free( level ); free( secs );
    if( want < 0 ) want = 0;
    if( want > 125 ) want = 125;
    char *out;   /* the playlist learns the new volume from the aout a moment later: answer the target */
    if( asprintf( &out, "{\"ok\":true,\"volume\":%d,\"was\":%d}", want, cur ) < 0 ) return NULL;
    return out;
}

static char *do_say( intf_sys_t *sys, const uint8_t *args )
{
    char *text = qarg( args, "text" );
    char *duck = qarg( args, "duck" );
    if( text == NULL ) { free( duck ); return json_error( "text= is required" ); }
    bool ok = announce( sys, text, duck ? atoi( duck ) : sys->duck );
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
            for( char *w = strtok_r( words, " ", &save ); w; w = strtok_r( NULL, " ", &save ) )
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
    char *q = qarg( args, "q" );
    char *coll = qarg( args, "collection" );
    char *rows = qarg( args, "rows" );
    if( q == NULL ) { free( coll ); free( rows ); return json_error( "q= is required" ); }
    int n = rows ? atoi( rows ) : 10;
    char *out = ia_search_json( sys, q, coll, n > 0 && n <= 50 ? n : 10 );
    free( q ); free( coll ); free( rows );
    return out;
}

static char *do_tracks( intf_sys_t *sys, const uint8_t *args )
{
    char *id = qarg( args, "id" );
    if( id == NULL ) return json_error( "id= is required: an archive.org item" );
    size_t n;
    iafile_t *f = ia_tracks( sys, id, &n );
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":true,\"play\":" );
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
    free( id );
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
        vlc_tick_t d = input_item_GetDuration( it->p_input );
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_printf( &ms, "{\"id\":%d,\"current\":%s,\"duration\":%" PRId64 ",",
                              it->i_id, it == cur ? "true" : "false", d > 0 ? d / CLOCK_FREQ : (vlc_tick_t)0 );
        jkv( &ms, "name", name, true );
        vlc_memstream_putc( &ms, '}' );
        free( name );
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
    /* the station's own feed, if it has one */
    char *uri = NULL;
    input_thread_t *in = playlist_CurrentInput( sys->playlist );
    if( in ) { uri = input_item_GetURI( input_GetItem( in ) ); vlc_object_release( in ); }
    const station_t *s = station_of_uri( sys, uri );
    free( uri );
    if( s == NULL || s->now == NULL ) return st;
    char *doc = json_retrieve_document( VLC_OBJECT(sys->intf), s->now );
    json_value *root = doc ? json_parse_document( VLC_OBJECT(sys->intf), doc ) : NULL;
    free( doc );
    if( root == NULL ) return st;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    st[strlen( st ) - 1] = '\0';   /* reopen the object */
    vlc_memstream_printf( &ms, "%s,\"recent\":[", st );
    free( st );
    const json_value *songs = json_getbyname( root, "songs" );
    if( songs && songs->type == json_array )
    {
        for( unsigned i = 0; i < songs->u.array.length && i < 4; i++ )
        {
            const json_value *sg = songs->u.array.values[i];
            char *line;
            const char *album = jsongetstring( sg, "album" );
            if( asprintf( &line, "%s - %s%s%s%s", jsongetstring( sg, "artist" ) ? jsongetstring( sg, "artist" ) : "?",
                          jsongetstring( sg, "title" ) ? jsongetstring( sg, "title" ) : "?",
                          album && *album ? " (" : "", album && *album ? album : "", album && *album ? ")" : "" ) < 0 ) continue;
            if( i ) vlc_memstream_putc( &ms, ',' );
            jstr( &ms, line );
            free( line );
        }
    }
    else if( jsongetstring( root, "artist" ) )
    {
        char *line;
        if( asprintf( &line, "%s - %s", jsongetstring( root, "artist" ),
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
    if( n <= 0 ) n = 20;
    FILE *f = log_open( sys->logpath, "rb" );
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

/*****************************************************************************
 * The HTTP server callback: one for every route
 *****************************************************************************/
static int Callback( httpd_callback_sys_t *p_sys, httpd_client_t *cl,
                     httpd_message_t *answer, const httpd_message_t *query )
{
    (void)cl;
    if( answer == NULL || query == NULL ) return VLC_SUCCESS;
    route_t *r = (route_t *)p_sys;
    intf_sys_t *sys = r->sys;
    const char *verb = r->path + strlen( "/dj/" );
    const uint8_t *args = query->psz_args;
    char *body = NULL;

    vlc_mutex_lock( &sys->lock );
    if( !strcmp( verb, "state" ) )         body = state_json( sys );
    else if( !strcmp( verb, "now" ) )      body = do_now( sys );
    else if( !strcmp( verb, "play" ) )     body = do_play( sys, args, true );
    else if( !strcmp( verb, "queue" ) )    body = do_play( sys, args, false );
    else if( !strcmp( verb, "next" ) )   { playlist_Next( sys->playlist ); dj_log( sys, "next", NULL ); msleep( CLOCK_FREQ / 2 ); body = state_json( sys ); }
    else if( !strcmp( verb, "prev" ) )   { playlist_Prev( sys->playlist ); dj_log( sys, "prev", NULL ); msleep( CLOCK_FREQ / 2 ); body = state_json( sys ); }
    else if( !strcmp( verb, "pause" ) )  { playlist_TogglePause( sys->playlist ); msleep( CLOCK_FREQ / 4 ); body = state_json( sys ); }
    else if( !strcmp( verb, "stop" ) )   { playlist_Stop( sys->playlist ); dj_log( sys, "stop", NULL ); msleep( CLOCK_FREQ / 4 ); body = state_json( sys ); }
    else if( !strcmp( verb, "clear" ) )  { playlist_Clear( sys->playlist, pl_Unlocked ); dj_log( sys, "clear", NULL ); body = state_json( sys ); }
    else if( !strcmp( verb, "remove" ) )   body = do_remove( sys, args );
    else if( !strcmp( verb, "volume" ) )   body = do_volume( sys, args );
    else if( !strcmp( verb, "say" ) )      body = do_say( sys, args );
    else if( !strcmp( verb, "stations" ) ) body = do_stations( sys, args );
    else if( !strcmp( verb, "search" ) )   body = do_search( sys, args );
    else if( !strcmp( verb, "tracks" ) )   body = do_tracks( sys, args );
    else if( !strcmp( verb, "playlist" ) ) body = do_playlist( sys );
    else if( !strcmp( verb, "log" ) )      body = do_log( sys, args );
    vlc_mutex_unlock( &sys->lock );

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
    "/dj/state", "/dj/now", "/dj/play", "/dj/queue", "/dj/next", "/dj/prev", "/dj/pause",
    "/dj/stop", "/dj/clear", "/dj/remove", "/dj/volume", "/dj/say", "/dj/stations",
    "/dj/search", "/dj/tracks", "/dj/playlist", "/dj/log",
};

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
    sys->fade = var_InheritFloat( intf, "dj-fade" );
    sys->password = var_InheritString( intf, "http-password" );
    vlc_mutex_init( &sys->lock );

    char *dir = config_GetUserDir( VLC_CONFIG_DIR );
    if( dir == NULL || asprintf( &sys->logpath, "%s" DIR_SEP "dj.log", dir ) < 0 ) sys->logpath = NULL;
    free( dir );

    if( sys->password == NULL || !*sys->password )
    {
        msg_Err( intf, "the DJ needs http-password set (it shares VLC's HTTP server)" );
        goto error;
    }
    sys->host = vlc_http_HostNew( obj );
    if( sys->host == NULL )
    {
        msg_Err( intf, "cannot start VLC's HTTP server (http-host, http-port)" );
        goto error;
    }
    sys->n_routes = sizeof paths / sizeof paths[0];
    sys->routes = calloc( sys->n_routes, sizeof *sys->routes );
    if( sys->routes == NULL ) goto error;
    for( size_t i = 0; i < sys->n_routes; i++ )
    {
        route_t *r = &sys->routes[i];
        r->sys = sys;
        r->path = paths[i];
        r->url = httpd_UrlNew( sys->host, paths[i], "", sys->password );
        if( r->url == NULL )
        {
            msg_Err( intf, "cannot register %s", paths[i] );
            goto error;
        }
        httpd_UrlCatch( r->url, HTTPD_MSG_GET, Callback, (httpd_callback_sys_t *)r );
    }
    msg_Info( intf, "DJ deck on VLC's HTTP server: /dj/state ... /dj/play?target=" );
    return VLC_SUCCESS;

error:
    Close( obj );
    return VLC_EGENERIC;
}

static void Close( vlc_object_t *obj )
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;
    if( sys->routes )
        for( size_t i = 0; i < sys->n_routes; i++ )
            if( sys->routes[i].url ) httpd_UrlDelete( sys->routes[i].url );
    free( sys->routes );
    if( sys->host ) httpd_HostDelete( sys->host );
    stations_free( sys );
    vlc_mutex_destroy( &sys->lock );
    free( sys->password );
    free( sys->logpath );
    free( sys );
}
