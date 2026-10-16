/*****************************************************************************
 * dj_ui.c : the DJ's deck window
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
 * The window a club DJ reads. Both records scroll past one playhead on one time scale, so
 * beats the mixer has matched line up on screen as they do in the ear. Under them, each
 * deck's tempo, pitch, time and source; between them, the mixer the DJ works: channel
 * faders, the bass EQ, the echo, the crossfader, the meters and the limiter. The next tracks,
 * the moods and a request box sit below, and every button is a verb the DJ already has.
 *
 * The plugin draws all of it into its own bitmap, thirty frames a second, on its own thread:
 * no toolkit stands between the engine's numbers and the screen. The look is a theme, a
 * preset with dj-theme.json's overrides on top; THEME cycles the presets.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef _WIN32
# include <winsock2.h>   /* before anything pulls in windows.h */
#endif

#include <vlc_common.h>
#include <vlc_fs.h>
#include <vlc_stream.h>
#include <vlc_configuration.h>

#include <math.h>
#include <time.h>

#include "../misc/webservices/json.h"
#include "../misc/webservices/json_helper.h"
#include "dj.h"

#ifndef _WIN32
/* drawn with GDI: elsewhere the DJ runs without a window */
dj_ui_t *dj_ui_New( vlc_object_t *obj, dj_mix_t *mix, dj_brain_cb brain, dj_command_cb cmd,
                    void *opaque, const char *theme_path )
{
    VLC_UNUSED(obj); VLC_UNUSED(mix); VLC_UNUSED(brain); VLC_UNUSED(cmd); VLC_UNUSED(opaque);
    VLC_UNUSED(theme_path);
    return NULL;
}
void dj_ui_Show( dj_ui_t *ui ) { VLC_UNUSED(ui); }
void dj_ui_Delete( dj_ui_t *ui ) { VLC_UNUSED(ui); }
#else

#include <windows.h>
#include <windowsx.h>

#ifndef WM_DPICHANGED
# define WM_DPICHANGED 0x02E0
#endif
#ifndef EM_SETCUEBANNER
# define EM_SETCUEBANNER 0x1501
#endif
#define WM_APP_SHOW   (WM_APP + 1)
#define WM_APP_QUIT   (WM_APP + 2)
#define CLASS_NAME    L"VLCDJDeck"
#define ID_EDIT       101
#define ID_FRAME      1
#define ID_PLACE      2
#define COUNT(a)      (sizeof (a) / sizeof *(a))
#define PI_F          3.14159265f

/*****************************************************************************
 * The theme
 *****************************************************************************/
enum { C_BG, C_PANEL, C_RAISED, C_EDGE, C_TEXT, C_DIM, C_FAINT, C_A, C_B, C_ACCENT, C_AIR,
       C_GOOD, C_WARN, C_BAD, C_HIGH, C_COUNT };

static const char *const colour_key[C_COUNT] =
{
    "background", "panel", "raised", "edge", "text", "dim", "faint", "deck_a", "deck_b",
    "accent", "on_air", "good", "warn", "bad", "highs",
};

static const struct { const char *name; uint32_t c[C_COUNT]; } presets[] =
{
    { "club",            { 0x07090d, 0x0e1219, 0x151b25, 0x232c3a, 0xe8edf4, 0x8591a4, 0x3a4455,
                           0x1fd2f5, 0xffad1f, 0x9b87ff, 0xff2d55, 0x2fe39a, 0xffd23f, 0xff4b4b, 0xf2fbff } },
    { "acid",            { 0x050705, 0x0b100b, 0x121a12, 0x1f2d1f, 0xeaf5e6, 0x88a382, 0x35472f,
                           0xb4ff1a, 0xffe41a, 0xff4fd8, 0xff2d2d, 0x5dff7a, 0xffe41a, 0xff4b4b, 0xfbfff0 } },
    { "berghain",        { 0x000000, 0x0a0a0a, 0x131313, 0x252525, 0xeeeeee, 0x8c8c8c, 0x3b3b3b,
                           0xf4f4f4, 0x9e9e9e, 0xd6d6d6, 0xff1e1e, 0xc4c4c4, 0xe6e6e6, 0xff3b3b, 0xffffff } },
    { "californication", { 0x110914, 0x1a0f1f, 0x24162b, 0x37243f, 0xfbeee6, 0xbb9cab, 0x58425f,
                           0xff7a52, 0xffd166, 0xef476f, 0xff3366, 0x7ce0a3, 0xffd166, 0xff5a5a, 0xfff6ea } },
};
#define N_PRESETS ((int)COUNT( presets ))

/* sizes at 96 dpi; faces: t the text face, d the digits face, i the icons */
enum { F_TITLE, F_BODY, F_SMALL, F_CAPS, F_BTN, F_XL, F_L, F_M, F_S, F_DECK, F_ICON, F_COUNT };
static const struct { int px, weight; char face; float extra; } font_spec[F_COUNT] =
{
    { 21, 600, 't', 0.f }, { 15, 400, 't', 0.f }, { 12, 400, 't', 0.f }, { 11, 700, 't', 1.4f },
    { 12, 600, 't', .5f }, { 40, 600, 'd', 0.f }, { 28, 400, 'd', 0.f }, { 18, 400, 'd', 0.f },
    { 13, 400, 'd', 0.f }, { 24, 700, 'd', 0.f }, { 15, 400, 'i', 0.f },
};

/*****************************************************************************
 * Types
 *****************************************************************************/
typedef struct
{
    HDC       dc;
    HBITMAP   bmp;
    HGDIOBJ   stock;
    uint32_t *px;
    int       w, h;
    int       cx0, cy0, cx1, cy1;   /* the clip */
    int       lx0, ly0, lx1, ly1;   /* the limit: the clip at its widest */
    bool      gdi;                  /* GDI drew since the last flush */
} canvas_t;

typedef struct { int x, y, w, h; } box_t;

enum { B_AIR = 1, B_VOICE, B_VOLDN, B_VOLUP, B_THEME, B_PLAY, B_QUEUE, B_NEXT, B_CHOOSE, B_MOOD };
typedef struct { box_t r; int id, arg; } button_t;

typedef struct { double pos; uint32_t track; bool ok; int bar; } follow_t;   /* a record's position, smoothed;
                                                                          the bar count it took over */
typedef struct { float db, peak; mtime_t peak_at; } meter_t;

struct dj_ui
{
    vlc_object_t  *obj;
    dj_mix_t      *mix;
    dj_brain_cb    brain_cb;
    dj_command_cb  command_cb;
    void          *opaque;
    char          *theme_path, *place_path;

    vlc_thread_t   thread;
    vlc_mutex_t    lock;
    vlc_cond_t     cond;
    int            up;                  /* 0 starting, 1 up, -1 failed; lock */
    HWND           hwnd, edit;

    /* the window thread's own */
    HINSTANCE      inst;
    HMODULE        dwm;
    WNDPROC        edit_proc;
    HBRUSH         edit_brush;
    HICON          icon_big, icon_small;
    bool           edit_focus, maximized, unplaced;
    UINT         (WINAPI *dpi_of)( HWND );
    unsigned       dpi;
    float          s;
    HFONT          font[F_COUNT];
    int            font_h[F_COUNT];
    int            adv[F_COUNT][128];
    canvas_t       chrome, mid, frame;   /* what holds still; what changes a few times a second; all */
    bool           chrome_dirty, mid_dirty;
    mtime_t        mid_at;
    uint32_t       c[C_COUNT];
    wchar_t        face_text[LF_FACESIZE], face_digits[LF_FACESIZE];
    int            preset, fps;
    box_t          head, waves, ov[DJ_DECKS], zoom[DJ_DECKS], deck[DJ_DECKS], mixer, next, moods, req,
                   field, status;
    box_t          fast[5];
    button_t       btn[64];
    int            n_btn;
    int            hover_id, hover_arg, press_id, press_arg, flash_id, flash_arg;
    mtime_t        flash_at, last_frame;
    float          zoom_s, spin;
    float         *wl, *wf;
    int            wcap;
    dj_status_t    st;
    bool           have_st;
    dj_brain_t     br;
    follow_t       follow[DJ_DECKS];
    meter_t        meter[DJ_DECKS], out;
    char           error_seen[240], said_seen[240];
    mtime_t        error_at, said_at;
    wchar_t        title[300];
};

static inline int S( const dj_ui_t *ui, float v ) { return (int)lroundf( v * ui->s ); }
static inline float clampf( float v, float lo, float hi ) { return v < lo ? lo : v > hi ? hi : v; }

/* 0xRRGGBB to a COLORREF */
static inline COLORREF cref( uint32_t c )
{
    return ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16);
}

/* a toward b */
static inline uint32_t mixc( uint32_t a, uint32_t b, float t )
{
    int k = (int)(t * 256.f + .5f);
    if( k <= 0 ) return a;
    if( k >= 256 ) return b;
    return ((((a & 0xff00ff) * (uint32_t)(256 - k) + (b & 0xff00ff) * (uint32_t)k) >> 8) & 0xff00ff)
         | ((((a & 0x00ff00) * (uint32_t)(256 - k) + (b & 0x00ff00) * (uint32_t)k) >> 8) & 0x00ff00);
}

static uint32_t hash( const char *s )
{
    uint32_t h = 2166136261u;
    for( ; s && *s; s++ ) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static bool parse_colour( const char *s, uint32_t *out )
{
    if( s == NULL ) return false;
    if( *s == '#' ) s++;
    size_t n = strlen( s );
    if( n != 6 && n != 3 ) return false;
    uint32_t v = 0;
    for( size_t i = 0; i < n; i++ )
    {
        int ch = s[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
              : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if( d < 0 ) return false;
        v = v << 4 | (uint32_t)d;
        if( n == 3 ) v = v << 4 | (uint32_t)d;
    }
    *out = v;
    return true;
}

static char *slurp( const char *path )
{
    FILE *f = path ? vlc_fopen( path, "rb" ) : NULL;
    if( f == NULL ) return NULL;
    char *buf = malloc( 65537 );
    size_t n = buf ? fread( buf, 1, 65536, f ) : 0;
    fclose( f );
    if( buf ) buf[n] = 0;
    return buf;
}

static json_value *theme_doc( const dj_ui_t *ui )
{
    char *text = slurp( ui->theme_path );
    if( text == NULL ) return NULL;
    json_settings settings;
    memset( &settings, 0, sizeof settings );
    char err[128];
    json_value *v = json_parse_ex( &settings, text, err );
    free( text );
    if( v && v->type != json_object )
    {
        json_value_free( v );
        v = NULL;
    }
    return v;
}

static void wide( const char *s, wchar_t *out, int cap )
{
    if( MultiByteToWideChar( CP_UTF8, 0, s, -1, out, cap ) <= 0 ) out[0] = 0;
}

/* a preset, dj-theme.json's colours and fonts over it; preset -1: the one the file names */
static void theme_load( dj_ui_t *ui, int preset )
{
    json_value *doc = theme_doc( ui );
    const char *want = doc ? jsongetstring( doc, "preset" ) : NULL;
    if( preset < 0 || preset >= N_PRESETS )
    {
        preset = 0;
        for( int i = 0; want && i < N_PRESETS; i++ )
            if( !strcasecmp( want, presets[i].name ) ) preset = i;
    }
    ui->preset = preset;
    memcpy( ui->c, presets[preset].c, sizeof ui->c );
    wcscpy( ui->face_text, L"Segoe UI" );
    wcscpy( ui->face_digits, L"Bahnschrift" );
    ui->fps = 30;
    if( doc == NULL ) return;
    for( int i = 0; i < C_COUNT; i++ )
        parse_colour( jsongetstring( doc, colour_key[i] ), &ui->c[i] );
    const char *f = jsongetstring( doc, "font" ), *g = jsongetstring( doc, "font_numbers" );
    if( f && *f ) wide( f, ui->face_text, LF_FACESIZE );
    if( g && *g ) wide( g, ui->face_digits, LF_FACESIZE );
    const json_value *fps = json_getbyname( doc, "fps" );
    if( fps && fps->type == json_integer ) ui->fps = VLC_CLIP( (int)fps->u.integer, 10, 60 );
    if( fps && fps->type == json_double ) ui->fps = VLC_CLIP( (int)fps->u.dbl, 10, 60 );
    json_value_free( doc );
}

static void json_put( FILE *f, const char *s )
{
    fputc( '"', f );
    for( const unsigned char *p = (const unsigned char *)s; *p; p++ )
        if( *p == '"' || *p == 0x5c ) { fputc( 0x5c, f ); fputc( *p, f ); }
        else if( *p < 0x20 ) fprintf( f, "%cu%04x", 0x5c, *p );
        else fputc( *p, f );
    fputc( '"', f );
}

/* THEME: the preset into dj-theme.json, the file's other keys kept */
static void theme_save( dj_ui_t *ui, int preset )
{
    if( ui->theme_path == NULL ) return;
    json_value *doc = theme_doc( ui );
    FILE *f = vlc_fopen( ui->theme_path, "wb" );
    if( f == NULL )
    {
        if( doc ) json_value_free( doc );
        return;
    }
    fputs( "{\n \"preset\": ", f );
    json_put( f, presets[preset].name );
    for( unsigned i = 0; doc && i < doc->u.object.length; i++ )
    {
        const char *k = doc->u.object.values[i].name;
        const json_value *v = doc->u.object.values[i].value;
        if( !strcmp( k, "preset" ) ) continue;
        char num[48] = "";
        if( v->type == json_integer ) snprintf( num, sizeof num, "%lld", (long long)v->u.integer );
        else if( v->type == json_double )
        {
            snprintf( num, sizeof num, "%.6g", v->u.dbl );
            for( char *c = num; *c; c++ ) if( *c == ',' ) *c = '.';
        }
        else if( v->type == json_boolean ) strcpy( num, v->u.boolean ? "true" : "false" );
        else if( v->type != json_string ) continue;
        fputs( ",\n ", f );
        json_put( f, k );
        fputs( ": ", f );
        if( v->type == json_string ) json_put( f, v->u.string.ptr );
        else fputs( num, f );
    }
    fputs( "\n}\n", f );
    fclose( f );
    if( doc ) json_value_free( doc );
}

/*****************************************************************************
 * The canvas: a top-down 32-bit DIB, shapes by signed distance, a pixel of soft edge
 *****************************************************************************/
static void canvas_free( canvas_t *cv )
{
    if( cv->dc )
    {
        if( cv->stock ) SelectObject( cv->dc, cv->stock );
        DeleteDC( cv->dc );
    }
    if( cv->bmp ) DeleteObject( cv->bmp );
    memset( cv, 0, sizeof *cv );
}

static void unclip( canvas_t *cv )
{
    cv->cx0 = cv->lx0;
    cv->cy0 = cv->ly0;
    cv->cx1 = cv->lx1;
    cv->cy1 = cv->ly1;
}

static void clip( canvas_t *cv, box_t b )
{
    cv->cx0 = __MAX( b.x, cv->lx0 );
    cv->cy0 = __MAX( b.y, cv->ly0 );
    cv->cx1 = __MIN( b.x + b.w, cv->lx1 );
    cv->cy1 = __MIN( b.y + b.h, cv->ly1 );
}

/* nothing drawn outside b, the text included, until unlimit */
static void limit( canvas_t *cv, box_t b )
{
    cv->lx0 = __MAX( b.x, 0 );
    cv->ly0 = __MAX( b.y, 0 );
    cv->lx1 = __MIN( b.x + b.w, cv->w );
    cv->ly1 = __MIN( b.y + b.h, cv->h );
    unclip( cv );
    HRGN rgn = CreateRectRgn( cv->lx0, cv->ly0, cv->lx1, cv->ly1 );
    SelectClipRgn( cv->dc, rgn );
    DeleteObject( rgn );
}

static void unlimit( canvas_t *cv )
{
    cv->lx0 = cv->ly0 = 0;
    cv->lx1 = cv->w;
    cv->ly1 = cv->h;
    unclip( cv );
    SelectClipRgn( cv->dc, NULL );
}

static bool canvas_size( canvas_t *cv, int w, int h )
{
    w = __MAX( w, 1 );
    h = __MAX( h, 1 );
    if( cv->px && cv->w == w && cv->h == h ) return true;
    BITMAPINFO bi;
    memset( &bi, 0, sizeof bi );
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection( NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0 );
    if( bmp == NULL ) return false;
    if( cv->dc == NULL ) cv->dc = CreateCompatibleDC( NULL );
    HGDIOBJ old = SelectObject( cv->dc, bmp );
    if( cv->stock == NULL ) cv->stock = old;
    if( cv->bmp ) DeleteObject( cv->bmp );
    cv->bmp = bmp;
    cv->px = bits;
    cv->w = w;
    cv->h = h;
    cv->gdi = false;
    SetBkMode( cv->dc, TRANSPARENT );
    unlimit( cv );
    return true;
}

static inline void flush( canvas_t *cv )
{
    if( cv->gdi ) { GdiFlush(); cv->gdi = false; }
}

/* a: 0..256 */
static inline void blend( uint32_t *p, uint32_t c, int a )
{
    if( a <= 0 ) return;
    if( a >= 256 ) { *p = c; return; }
    uint32_t d = *p;
    *p = ((((d & 0xff00ff) * (uint32_t)(256 - a) + (c & 0xff00ff) * (uint32_t)a) >> 8) & 0xff00ff)
       | ((((d & 0x00ff00) * (uint32_t)(256 - a) + (c & 0x00ff00) * (uint32_t)a) >> 8) & 0x00ff00);
}

static bool bounds( canvas_t *cv, float x0, float y0, float x1, float y1,
                    int *ax, int *ay, int *bx, int *by )
{
    *ax = __MAX( (int)floorf( x0 ), cv->cx0 );
    *ay = __MAX( (int)floorf( y0 ), cv->cy0 );
    *bx = __MIN( (int)ceilf( x1 ), cv->cx1 );
    *by = __MIN( (int)ceilf( y1 ), cv->cy1 );
    if( *ax >= *bx || *ay >= *by ) return false;
    flush( cv );
    return true;
}

#define EACH_PIXEL \
    for( int py = ay; py < by; py++ ) \
        for( uint32_t *row = cv->px + (size_t)py * cv->w, *once = row; once; once = NULL ) \
            for( int px = ax; px < bx; px++ )

#define COVER( d, c, alpha ) do { \
        float cov_ = clampf( .5f - (d), 0.f, 1.f ); \
        if( cov_ > 0.f ) blend( row + px, (c), (int)(cov_ * (alpha) * 256.f) ); \
    } while( 0 )

/* to the subpixel */
static void rect( canvas_t *cv, float x0, float y0, float x1, float y1, uint32_t c, float alpha )
{
    int ax, ay, bx, by;
    if( alpha <= 0.f || !bounds( cv, x0, y0, x1, y1, &ax, &ay, &bx, &by ) ) return;
    EACH_PIXEL
    {
        float cy = clampf( fminf( py + 1.f, y1 ) - fmaxf( (float)py, y0 ), 0.f, 1.f );
        float cx = clampf( fminf( px + 1.f, x1 ) - fmaxf( (float)px, x0 ), 0.f, 1.f );
        blend( row + px, c, (int)(cx * cy * alpha * 256.f) );
    }
}

static void column( canvas_t *cv, int x, float y0, float y1, uint32_t c, float alpha )
{
    if( x < cv->cx0 || x >= cv->cx1 || y1 <= y0 || alpha <= 0.f ) return;
    flush( cv );
    int a = (int)(alpha * 256.f), ya = (int)floorf( y0 ), yb = (int)ceilf( y1 );
    uint32_t *p = cv->px + x;
    for( int y = __MAX( ya, cv->cy0 ), end = __MIN( yb, cv->cy1 ); y < end; y++ )
    {
        float cov = y == ya || y == yb - 1 ? clampf( fminf( y + 1.f, y1 ) - fmaxf( (float)y, y0 ), 0.f, 1.f ) : 1.f;
        blend( p + (size_t)y * cv->w, c, cov >= 1.f ? a : (int)(cov * a) );
    }
}

static void vgrad( canvas_t *cv, int x0, int y0, int x1, int y1, uint32_t top, uint32_t bottom )
{
    int ax, ay, bx, by;
    if( !bounds( cv, x0, y0, x1, y1, &ax, &ay, &bx, &by ) ) return;
    for( int py = ay; py < by; py++ )
    {
        uint32_t c = mixc( top, bottom, y1 - y0 > 1 ? (float)(py - y0) / (float)(y1 - y0 - 1) : 0.f );
        uint32_t *row = cv->px + (size_t)py * cv->w;
        for( int px = ax; px < bx; px++ ) row[px] = c;
    }
}

static inline float sd_rrect( float px, float py, float cx, float cy, float hx, float hy, float r )
{
    float qx = fabsf( px - cx ) - hx + r, qy = fabsf( py - cy ) - hy + r;
    if( qx <= 0.f && qy <= 0.f ) return fmaxf( qx, qy ) - r;
    float ox = fmaxf( qx, 0.f ), oy = fmaxf( qy, 0.f );
    return sqrtf( ox * ox + oy * oy ) + fminf( fmaxf( qx, qy ), 0.f ) - r;
}

/* filled when stroke is 0, else a line that wide inside the edge; top to bottom from c to c2 */
static void rrect2( canvas_t *cv, float x, float y, float w, float h, float r, float stroke,
                    uint32_t c, uint32_t c2, float alpha )
{
    int ax, ay, bx, by;
    if( alpha <= 0.f || w <= 0.f || h <= 0.f || !bounds( cv, x, y, x + w, y + h, &ax, &ay, &bx, &by ) )
        return;
    float hx = w / 2.f, hy = h / 2.f, cx = x + hx, cy = y + hy;
    r = fminf( r, fminf( hx, hy ) );
    EACH_PIXEL
    {
        float d = sd_rrect( px + .5f, py + .5f, cx, cy, hx, hy, r );
        if( stroke > 0.f ) d = fabsf( d + stroke / 2.f ) - stroke / 2.f;
        COVER( d, c == c2 ? c : mixc( c, c2, (py + .5f - y) / h ), alpha );
    }
}

static void rrect( canvas_t *cv, float x, float y, float w, float h, float r, float stroke,
                   uint32_t c, float alpha )
{
    rrect2( cv, x, y, w, h, r, stroke, c, c, alpha );
}

/* soft, a little below the box it is under */
static void shadow( canvas_t *cv, float x, float y, float w, float h, float r, float spread, float alpha )
{
    int ax, ay, bx, by;
    if( !bounds( cv, x - spread, y - spread / 2.f, x + w + spread, y + h + spread * 1.5f, &ax, &ay, &bx, &by ) )
        return;
    float hx = w / 2.f, hy = h / 2.f, cx = x + hx, cy = y + hy + spread / 2.f;
    EACH_PIXEL
    {
        float d = sd_rrect( px + .5f, py + .5f, cx, cy, hx, hy, r );
        if( d >= spread ) continue;
        float t = d <= 0.f ? 1.f : 1.f - d / spread;
        blend( row + px, 0, (int)(t * t * alpha * 256.f) );
    }
}

static void disc( canvas_t *cv, float cx, float cy, float r, uint32_t c, float alpha )
{
    int ax, ay, bx, by;
    if( alpha <= 0.f || !bounds( cv, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &ax, &ay, &bx, &by ) )
        return;
    EACH_PIXEL
    {
        float dx = px + .5f - cx, dy = py + .5f - cy;
        COVER( sqrtf( dx * dx + dy * dy ) - r, c, alpha );
    }
}

static void glow( canvas_t *cv, float cx, float cy, float r, uint32_t c, float alpha )
{
    int ax, ay, bx, by;
    if( alpha <= 0.f || !bounds( cv, cx - r, cy - r, cx + r, cy + r, &ax, &ay, &bx, &by ) ) return;
    float r2 = 1.f / (r * r);
    EACH_PIXEL
    {
        float dx = px + .5f - cx, dy = py + .5f - cy, t = 1.f - (dx * dx + dy * dy) * r2;
        if( t > 0.f ) blend( row + px, c, (int)(t * t * t * alpha * 256.f) );
    }
}

/* an arc of a ring with round ends, a0 to a1 clockwise from twelve o'clock */
static void arc( canvas_t *cv, float cx, float cy, float r, float w, float a0, float a1,
                 uint32_t c, float alpha )
{
    int ax, ay, bx, by;
    float R = r + w;
    if( alpha <= 0.f || a1 <= a0 || !bounds( cv, cx - R - 1, cy - R - 1, cx + R + 1, cy + R + 1, &ax, &ay, &bx, &by ) )
        return;
    bool whole = a1 - a0 >= 2.f * PI_F;
    float e0x = cx + r * sinf( a0 ), e0y = cy - r * cosf( a0 );
    float e1x = cx + r * sinf( a1 ), e1y = cy - r * cosf( a1 );
    EACH_PIXEL
    {
        float x = px + .5f, y = py + .5f, dx = x - cx, dy = y - cy, d;
        bool in = whole || fmodf( atan2f( dx, -dy ) - a0 + 4.f * PI_F, 2.f * PI_F ) <= a1 - a0;
        if( in ) d = fabsf( sqrtf( dx * dx + dy * dy ) - r ) - w / 2.f;
        else d = fminf( hypotf( x - e0x, y - e0y ), hypotf( x - e1x, y - e1y ) ) - w / 2.f;
        COVER( d, c, alpha );
    }
}

/* round ends */
static void line( canvas_t *cv, float x0, float y0, float x1, float y1, float w, uint32_t c, float alpha )
{
    int ax, ay, bx, by;
    float h = w / 2.f + 1.f;
    if( alpha <= 0.f || !bounds( cv, fminf( x0, x1 ) - h, fminf( y0, y1 ) - h, fmaxf( x0, x1 ) + h,
                                 fmaxf( y0, y1 ) + h, &ax, &ay, &bx, &by ) )
        return;
    float vx = x1 - x0, vy = y1 - y0, ll = vx * vx + vy * vy;
    EACH_PIXEL
    {
        float x = px + .5f - x0, y = py + .5f - y0;
        float t = ll > 0.f ? clampf( (x * vx + y * vy) / ll, 0.f, 1.f ) : 0.f;
        COVER( hypotf( x - t * vx, y - t * vy ) - w / 2.f, c, alpha );
    }
}

static void tri( canvas_t *cv, float x0, float y0, float x1, float y1, float x2, float y2,
                 uint32_t c, float alpha )
{
    int ax, ay, bx, by;
    if( alpha <= 0.f || !bounds( cv, fminf( x0, fminf( x1, x2 ) ), fminf( y0, fminf( y1, y2 ) ),
                                 fmaxf( x0, fmaxf( x1, x2 ) ) + 1, fmaxf( y0, fmaxf( y1, y2 ) ) + 1,
                                 &ax, &ay, &bx, &by ) )
        return;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if( fabsf( area ) < 1e-3f ) return;
    float sg = area > 0.f ? 1.f : -1.f;
    const float ex[3] = { x0, x1, x2 }, ey[3] = { y0, y1, y2 };
    float len[3];
    for( int i = 0; i < 3; i++ )
        len[i] = hypotf( ex[(i + 1) % 3] - ex[i], ey[(i + 1) % 3] - ey[i] );
    EACH_PIXEL
    {
        float x = px + .5f, y = py + .5f, in = 1e9f;
        for( int i = 0; i < 3; i++ )
        {
            float e = ((ex[(i + 1) % 3] - ex[i]) * (y - ey[i]) - (ey[(i + 1) % 3] - ey[i]) * (x - ex[i]))
                      / len[i] * sg;
            in = fminf( in, e );
        }
        COVER( -in, c, alpha );
    }
}

/* the DJ's record at angle ang: grooves, a label in the two decks' colours, a sheen; the colour
   and the coverage of the point dx, dy from the centre of one of radius R */
static uint32_t vinyl_px( const uint32_t *c, float dx, float dy, float R, float ang, float *cov )
{
    float d = sqrtf( dx * dx + dy * dy ), hole = R * .08f, label = R * .40f;
    *cov = clampf( R - d + .5f, 0.f, 1.f ) * clampf( d - hole + .5f, 0.f, 1.f );
    if( d < label + .5f )
    {
        uint32_t l = mixc( c[C_A], c[C_B], clampf( dx + .5f, 0.f, 1.f ) );
        return mixc( l, 0x0d0f14, clampf( d - label + .5f, 0.f, 1.f ) );
    }
    float wl = R * .085f;
    float g = wl >= 2.f ? .5f + .5f * sinf( d * 2.f * PI_F / wl ) : .5f;
    uint32_t v = mixc( 0x0d0f14, 0x262b34, .55f * g );
    float sheen = powf( fmaxf( 0.f, cosf( 2.f * (atan2f( dy, dx ) - ang) ) ), 6.f ) * .32f;
    return mixc( v, 0xa8b2c0, sheen );
}

static void vinyl( canvas_t *cv, float cx, float cy, float R, float ang, const uint32_t *c )
{
    int ax, ay, bx, by;
    if( !bounds( cv, cx - R - 1, cy - R - 1, cx + R + 1, cy + R + 1, &ax, &ay, &bx, &by ) ) return;
    EACH_PIXEL
    {
        float cov;
        uint32_t v = vinyl_px( c, px + .5f - cx, py + .5f - cy, R, ang, &cov );
        if( cov > 0.f ) blend( row + px, v, (int)(cov * 256.f) );
    }
}

/*****************************************************************************
 * Text: GDI into the same bitmap, ClearType over what is already drawn
 *****************************************************************************/
enum { AL_LEFT, AL_CENTER, AL_RIGHT };

static void fonts_free( dj_ui_t *ui )
{
    for( int i = 0; i < F_COUNT; i++ )
        if( ui->font[i] )
        {
            DeleteObject( ui->font[i] );
            ui->font[i] = NULL;
        }
}

static HFONT font_new( const dj_ui_t *ui, int i, const wchar_t *face )
{
    return CreateFontW( -S( ui, font_spec[i].px ), 0, 0, 0, font_spec[i].weight, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, face );
}

static void fonts_make( dj_ui_t *ui )
{
    fonts_free( ui );
    HDC dc = CreateCompatibleDC( NULL );
    for( int i = 0; i < F_COUNT; i++ )
    {
        char kind = font_spec[i].face;
        ui->font[i] = font_new( ui, i, kind == 'd' ? ui->face_digits : kind == 'i' ? L"Segoe Fluent Icons"
                                                                     : ui->face_text );
        HGDIOBJ old = SelectObject( dc, ui->font[i] );
        if( kind == 'i' )
        {
            wchar_t got[LF_FACESIZE] = L"";
            GetTextFaceW( dc, LF_FACESIZE, got );
            if( wcscmp( got, L"Segoe Fluent Icons" ) )   /* Windows 10 */
            {
                SelectObject( dc, old );
                DeleteObject( ui->font[i] );
                ui->font[i] = font_new( ui, i, L"Segoe MDL2 Assets" );
                SelectObject( dc, ui->font[i] );
            }
        }
        TEXTMETRICW tm;
        ui->font_h[i] = GetTextMetricsW( dc, &tm ) ? tm.tmHeight : S( ui, font_spec[i].px * 1.3f );
        INT w[128];
        if( GetCharWidth32W( dc, 0, 127, w ) )
            for( int k = 0; k < 128; k++ ) ui->adv[i][k] = w[k];
        SelectObject( dc, old );
    }
    DeleteDC( dc );
}

static void use_font( dj_ui_t *ui, canvas_t *cv, int f, uint32_t c )
{
    SelectObject( cv->dc, ui->font[f] );
    SetTextColor( cv->dc, cref( c ) );
    SetTextCharacterExtra( cv->dc, (int)lroundf( font_spec[f].extra * ui->s ) );
    cv->gdi = true;
}

static int utf16( const char *s, wchar_t *out, int cap )
{
    int n = s && *s ? MultiByteToWideChar( CP_UTF8, 0, s, -1, out, cap ) : 0;
    if( n <= 0 ) { out[0] = 0; return 0; }
    return n - 1;
}

/* one line in w from x, y; an ellipsis when it does not fit */
static void text( dj_ui_t *ui, canvas_t *cv, int f, const char *s, int x, int y, int w,
                  uint32_t c, int align )
{
    wchar_t buf[512];
    int n = utf16( s, buf, (int)COUNT( buf ) );
    if( n == 0 || w <= 0 ) return;
    use_font( ui, cv, f, c );
    if( font_spec[f].extra > 0.f )   /* letter-spaced: DrawText clips the spacing it does not measure */
    {
        SIZE sz;
        if( GetTextExtentPoint32W( cv->dc, buf, n, &sz ) && sz.cx <= w )
        {
            TextOutW( cv->dc, align == AL_CENTER ? x + (w - sz.cx) / 2 : align == AL_RIGHT ? x + w - sz.cx : x, y, buf, n );
            return;
        }
        SetTextCharacterExtra( cv->dc, 0 );
    }
    RECT r = { x, y, x + w, y + ui->font_h[f] + 2 };
    DrawTextW( cv->dc, buf, n, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_TOP
               | (align == AL_CENTER ? DT_CENTER : align == AL_RIGHT ? DT_RIGHT : DT_LEFT) );
}

static int text_w( dj_ui_t *ui, canvas_t *cv, int f, const char *s )
{
    wchar_t buf[512];
    int n = utf16( s, buf, (int)COUNT( buf ) );
    if( n == 0 ) return 0;
    SelectObject( cv->dc, ui->font[f] );
    SetTextCharacterExtra( cv->dc, (int)lroundf( font_spec[f].extra * ui->s ) );
    SIZE sz;
    return GetTextExtentPoint32W( cv->dc, buf, n, &sz ) ? sz.cx : 0;
}

/* digits each as wide as the widest, so a running number holds still; x is the left edge, the
   centre or the right edge as aligned; ASCII; the width drawn */
static int num( dj_ui_t *ui, canvas_t *cv, int f, const char *s, int x, int y, uint32_t c, int align )
{
    int cell = 0, total = 0, n = (int)strlen( s );
    for( int d = '0'; d <= '9'; d++ ) cell = __MAX( cell, ui->adv[f][d] );
    for( int i = 0; i < n; i++ )
        total += s[i] >= '0' && s[i] <= '9' ? cell : ui->adv[f][(unsigned char)s[i] & 127];
    if( align == AL_CENTER ) x -= total / 2;
    else if( align == AL_RIGHT ) x -= total;
    use_font( ui, cv, f, c );
    SetTextCharacterExtra( cv->dc, 0 );
    for( int i = 0; i < n; i++ )
    {
        wchar_t ch = (unsigned char)s[i] & 127;
        int adv = ui->adv[f][ch];
        bool digit = ch >= '0' && ch <= '9';
        TextOutW( cv->dc, digit ? x + (cell - adv) / 2 : x, y, &ch, 1 );
        x += digit ? cell : adv;
    }
    return total;
}

static void glyph( dj_ui_t *ui, canvas_t *cv, wchar_t g, float cx, float cy, uint32_t c )
{
    use_font( ui, cv, F_ICON, c );
    SIZE sz;
    if( GetTextExtentPoint32W( cv->dc, &g, 1, &sz ) )
        TextOutW( cv->dc, (int)lroundf( cx - sz.cx / 2.f ), (int)lroundf( cy - sz.cy / 2.f ), &g, 1 );
}

/* a place where a click does something, for this frame */
static void hot( dj_ui_t *ui, box_t r, int id, int arg )
{
    if( ui->n_btn < (int)COUNT( ui->btn ) )
        ui->btn[ui->n_btn++] = (button_t){ r, id, arg };
}

enum { ST_OUTLINE, ST_FILL };

static void button( dj_ui_t *ui, canvas_t *cv, box_t r, const char *label, wchar_t g, int id, int arg,
                    int style, uint32_t col )
{
    const uint32_t *c = ui->c;
    bool hover = ui->hover_id == id && ui->hover_arg == arg;
    bool down = (hover && ui->press_id == id && ui->press_arg == arg)
             || (ui->flash_id == id && ui->flash_arg == arg && mdate() - ui->flash_at < 200000);
    float rad = r.h / 2.f;
    uint32_t fg;
    if( style == ST_FILL )
    {
        rrect( cv, r.x, r.y, r.w, r.h, rad, 0, mixc( col, 0xffffff, down ? .28f : hover ? .14f : 0.f ), 1.f );
        fg = c[C_BG];
    }
    else
    {
        if( down || hover ) rrect( cv, r.x, r.y, r.w, r.h, rad, 0, col, down ? .32f : .12f );
        rrect( cv, r.x, r.y, r.w, r.h, rad, ui->s, col, hover ? .95f : .5f );
        fg = mixc( col, c[C_TEXT], hover || down ? .45f : .25f );
    }
    hot( ui, r, id, arg );
    if( !*label )
    {
        glyph( ui, cv, g, r.x + r.w / 2.f, r.y + r.h / 2.f, fg );
        return;
    }
    int tw = text_w( ui, cv, F_BTN, label ), gw = g ? S( ui, 22 ) : 0;
    int x = r.x + (r.w - tw - gw) / 2;
    if( g ) glyph( ui, cv, g, x + S( ui, 8 ), r.y + r.h / 2.f, fg );
    text( ui, cv, F_BTN, label, x + gw, r.y + (r.h - ui->font_h[F_BTN]) / 2, tw + S( ui, 4 ), fg, AL_LEFT );
}

static const float seg_db[12] = { -30, -24, -18, -14, -10, -8, -6, -4, -3, -2, -1, 0 };

/* LED segments, bottom up: unlit for the chrome; lit, what the level reaches and the held peak */
static void meter( dj_ui_t *ui, canvas_t *cv, float x, float top, float bot, float w, float db,
                   float peak, bool lit )
{
    float gap = 2.f * ui->s, h = (bot - top - 11.f * gap) / 12.f, r = 1.5f * ui->s;
    for( int i = 0; i < 12; i++ )
    {
        float y0 = bot - (i + 1) * h - i * gap;
        uint32_t col = ui->c[i >= 10 ? C_BAD : i >= 7 ? C_WARN : C_GOOD];
        if( !lit ) rrect( cv, x, y0, w, h, r, 0, col, .13f );
        else if( db >= seg_db[i] ) rrect( cv, x, y0, w, h, r, 0, col, 1.f );
        else if( peak >= seg_db[i] && (i == 11 || peak < seg_db[i + 1]) ) rrect( cv, x, y0, w, h, r, 0, col, .55f );
    }
}

/*****************************************************************************
 * Layout and chrome: what does not move, drawn once a size and a theme
 *****************************************************************************/
static void layout( dj_ui_t *ui )
{
    int W = ui->frame.w, H = ui->frame.h, pad = S( ui, 12 ), gap = S( ui, 10 );
    ui->head = (box_t){ 0, 0, W, S( ui, 56 ) };
    ui->status = (box_t){ 0, H - S( ui, 28 ), W, S( ui, 28 ) };
    int bottom_h = S( ui, 156 ), deck_h = S( ui, 218 );
    int by = ui->status.y - gap - bottom_h, dy = by - gap - deck_h, wy = ui->head.h + gap;
    int wh = __MAX( dy - gap - wy, S( ui, 120 ) );
    ui->waves = (box_t){ pad, wy, W - 2 * pad, wh };
    int in = S( ui, 8 ), ovh = S( ui, 30 ), sg = S( ui, 6 );
    int zx = ui->waves.x + in, zw = ui->waves.w - 2 * in;
    int zh = __MAX( (wh - 2 * in - 2 * ovh - 3 * sg) / 2, S( ui, 20 ) );
    ui->ov[0] = (box_t){ zx, wy + in, zw, ovh };
    ui->zoom[0] = (box_t){ zx, ui->ov[0].y + ovh + sg, zw, zh };
    ui->zoom[1] = (box_t){ zx, ui->zoom[0].y + zh + sg, zw, zh };
    ui->ov[1] = (box_t){ zx, ui->zoom[1].y + zh + sg, zw, ovh };
    int mw = S( ui, 320 ), dw = (W - 2 * pad - 2 * gap - mw) / 2;
    ui->deck[0] = (box_t){ pad, dy, dw, deck_h };
    ui->mixer = (box_t){ pad + dw + gap, dy, mw, deck_h };
    ui->deck[1] = (box_t){ ui->mixer.x + mw + gap, dy, W - pad - ui->mixer.x - mw - gap, deck_h };
    int all = W - 2 * pad - 2 * gap, nw = all * 3 / 10, rw = __MAX( S( ui, 340 ), all * 3 / 10 );
    ui->next = (box_t){ pad, by, nw, bottom_h };
    ui->req = (box_t){ W - pad - rw, by, rw, bottom_h };
    ui->moods = (box_t){ pad + nw + gap, by, ui->req.x - 2 * gap - pad - nw, bottom_h };
    ui->field = (box_t){ ui->req.x + S( ui, 14 ), by + S( ui, 34 ), rw - S( ui, 28 ), S( ui, 34 ) };
    ui->fast[0] = (box_t){ 0, 0, __MIN( S( ui, 330 ), W ), ui->head.h };
    ui->fast[1] = ui->waves;
    ui->fast[2] = ui->mixer;
    for( int d = 0; d < DJ_DECKS; d++ )
        ui->fast[3 + d] = (box_t){ ui->deck[d].x + S( ui, 2 ), ui->deck[d].y + ui->deck[d].h - S( ui, 48 ),
                                   S( ui, 136 ), S( ui, 46 ) };
    if( ui->edit )
    {
        int eh = ui->font_h[F_BODY] + S( ui, 2 );
        SetWindowPos( ui->edit, NULL, ui->field.x + S( ui, 12 ), ui->field.y + (ui->field.h - eh) / 2,
                      ui->field.w - S( ui, 24 ), eh, SWP_NOZORDER | SWP_NOACTIVATE );
    }
    ui->chrome_dirty = true;
}

/* channel ch's knob, fader and meter; the fader's travel and the meters, top to bottom */
static void strip_geom( const dj_ui_t *ui, int ch, float *kx, float *ky, float *fx, float *top,
                        float *bot, float *mx )
{
    box_t m = ui->mixer;
    float s = ui->s;
    *ky = m.y + 50.f * s;
    *top = m.y + 92.f * s;
    *bot = m.y + 172.f * s;
    *kx = *fx = ch == 0 ? m.x + 46.f * s : m.x + m.w - 46.f * s;
    *mx = ch == 0 ? m.x + 96.f * s : m.x + m.w - 106.f * s;
}

static void panel( dj_ui_t *ui, canvas_t *cv, box_t b, uint32_t fill, uint32_t accent )
{
    float r = 10.f * ui->s;
    shadow( cv, b.x, b.y, b.w, b.h, r, 12.f * ui->s, .5f );
    rrect2( cv, b.x, b.y, b.w, b.h, r, 0, mixc( fill, 0xffffff, .03f ), fill, 1.f );
    rrect( cv, b.x, b.y, b.w, b.h, r, 1.f, ui->c[C_EDGE], .9f );
    if( accent ) rrect( cv, b.x + r, b.y, b.w - 2 * r, 2.f * ui->s, ui->s, 0, accent, .95f );
}

static void caption( dj_ui_t *ui, canvas_t *cv, box_t b, const char *s )
{
    text( ui, cv, F_CAPS, s, b.x + S( ui, 14 ), b.y + S( ui, 12 ), b.w - S( ui, 28 ), ui->c[C_DIM], AL_LEFT );
}

static void mixer_chrome( dj_ui_t *ui, canvas_t *cv )
{
    box_t m = ui->mixer;
    float s = ui->s;
    const uint32_t *c = ui->c;
    text( ui, cv, F_CAPS, "MIXER", m.x, m.y + S( ui, 12 ), m.w, c[C_DIM], AL_CENTER );
    for( int d = 0; d < DJ_DECKS; d++ )
    {
        float kx, ky, fx, top, bot, mx;
        strip_geom( ui, d, &kx, &ky, &fx, &top, &bot, &mx );
        arc( cv, kx, ky, 19.f * s, 3.f * s, -2.356f, 2.356f, c[C_FAINT], .8f );   /* the knob's travel */
        text( ui, cv, F_CAPS, "LOW", (int)(kx - 30.f * s), (int)(ky + 23.f * s), S( ui, 60 ), c[C_DIM], AL_CENTER );
        rrect( cv, fx - 3.f * s, top - 4.f * s, 6.f * s, bot - top + 8.f * s, 3.f * s, 0, 0, .6f );
        for( int i = 0; i <= 4; i++ )
        {
            float y = bot - i * (bot - top) / 4.f;
            rect( cv, fx - 17.f * s, y - .5f, fx - 10.f * s, y + .5f, c[C_FAINT], 1.f );
            rect( cv, fx + 10.f * s, y - .5f, fx + 17.f * s, y + .5f, c[C_FAINT], 1.f );
        }
        meter( ui, cv, mx, top, bot, 10.f * s, -200.f, -200.f, false );
        text( ui, cv, F_BTN, d ? "B" : "A", (int)(mx - 10.f * s), (int)(top - 18.f * s), S( ui, 30 ),
              c[d ? C_B : C_A], AL_CENTER );
    }
    float cx = m.x + m.w / 2.f, top = m.y + 92.f * s, bot = m.y + 172.f * s;
    text( ui, cv, F_CAPS, "LIM", (int)(cx - 38.f * s), (int)(top - 17.f * s), S( ui, 40 ), c[C_DIM], AL_CENTER );
    text( ui, cv, F_CAPS, "OUT", (int)(cx - 2.f * s), (int)(top - 17.f * s), S( ui, 40 ), c[C_DIM], AL_CENTER );
    rrect( cv, cx - 22.f * s, top, 8.f * s, bot - top, 2.f * s, 0, 0, .55f );
    meter( ui, cv, cx + 14.f * s, top, bot, 8.f * s, -200.f, -200.f, false );
    float y = m.y + 198.f * s, x0 = m.x + 74.f * s, x1 = m.x + m.w - 74.f * s;
    rrect( cv, x0 - 4.f * s, y - 3.f * s, x1 - x0 + 8.f * s, 6.f * s, 3.f * s, 0, 0, .6f );
    rect( cv, cx - .5f, y - 12.f * s, cx + .5f, y - 6.f * s, c[C_FAINT], 1.f );
    rect( cv, cx - .5f, y + 6.f * s, cx + .5f, y + 12.f * s, c[C_FAINT], 1.f );
    int lh = ui->font_h[F_BTN];
    text( ui, cv, F_BTN, "A", (int)(x0 - 34.f * s), (int)(y - lh / 2.f), S( ui, 20 ), c[C_A], AL_CENTER );
    text( ui, cv, F_BTN, "B", (int)(x1 + 14.f * s), (int)(y - lh / 2.f), S( ui, 20 ), c[C_B], AL_CENTER );
}

static void chrome( dj_ui_t *ui )
{
    canvas_t *cv = &ui->chrome;
    const uint32_t *c = ui->c;
    float s = ui->s;
    unclip( cv );
    vgrad( cv, 0, 0, cv->w, cv->h, mixc( c[C_BG], c[C_PANEL], .5f ), c[C_BG] );
    vgrad( cv, 0, 0, cv->w, ui->head.h, c[C_RAISED], mixc( c[C_RAISED], c[C_BG], .6f ) );
    rect( cv, 0, ui->head.h - 1, cv->w, ui->head.h, c[C_EDGE], 1.f );
    text( ui, cv, F_TITLE, "VLC DJ", S( ui, 54 ), (ui->head.h - ui->font_h[F_TITLE]) / 2, S( ui, 90 ),
          c[C_TEXT], AL_LEFT );
    panel( ui, cv, ui->waves, c[C_PANEL], 0 );
    for( int d = 0; d < DJ_DECKS; d++ )
    {
        box_t z = ui->zoom[d], o = ui->ov[d];
        rrect( cv, z.x, z.y, z.w, z.h, 6.f * s, 0, mixc( c[C_BG], c[C_PANEL], .25f ), 1.f );
        rrect( cv, o.x, o.y, o.w, o.h, 5.f * s, 0, mixc( c[C_BG], c[C_PANEL], .5f ), 1.f );
        rect( cv, z.x, z.y + z.h / 2.f - .5f, z.x + z.w, z.y + z.h / 2.f + .5f, c[C_FAINT], .45f );
    }
    for( int d = 0; d < DJ_DECKS; d++ ) panel( ui, cv, ui->deck[d], c[C_PANEL], c[d ? C_B : C_A] );
    panel( ui, cv, ui->mixer, c[C_RAISED], 0 );
    mixer_chrome( ui, cv );
    panel( ui, cv, ui->next, c[C_PANEL], 0 );
    panel( ui, cv, ui->moods, c[C_PANEL], 0 );
    panel( ui, cv, ui->req, c[C_PANEL], 0 );
    caption( ui, cv, ui->next, "NEXT UP" );
    caption( ui, cv, ui->moods, "MOODS" );
    caption( ui, cv, ui->req, "REQUEST" );
    rect( cv, 0, ui->status.y, cv->w, cv->h, c[C_PANEL], 1.f );
    rect( cv, 0, ui->status.y, cv->w, ui->status.y + 1, c[C_EDGE], 1.f );
    flush( cv );
    ui->chrome_dirty = false;
}

/*****************************************************************************
 * The records
 *****************************************************************************/
static bool wave_buf( dj_ui_t *ui, int n )
{
    if( n <= ui->wcap ) return true;
    float *l = realloc( ui->wl, n * sizeof *l ), *f = l ? realloc( ui->wf, n * sizeof *f ) : NULL;
    if( l ) ui->wl = l;
    if( f ) ui->wf = f;
    if( l == NULL || f == NULL ) return false;
    ui->wcap = n;
    return true;
}

/* amplitude to height: full scale in a millisecond fills it, 30 dB under is nothing */
static inline float wave_h( float v, float gain )
{
    if( v <= 0.f ) return 0.f;
    float t = (20.f * log10f( v * gain ) + 30.f) / 30.f;
    return t <= 0.f ? 0.f : t >= 1.f ? 1.f : t * sqrtf( t );
}

/* a column of the record: the whole band, whiter the less of it is bass, the kick band inside */
static inline void wave_col( dj_ui_t *ui, canvas_t *cv, int d, float x, float yc, float half,
                             float low, float full, float gain, float a )
{
    if( full < 0.f ) return;
    uint32_t dc = ui->c[d ? C_B : C_A];
    float hf = wave_h( full, gain ) * half, hl = fminf( wave_h( low, gain ) * half, hf );
    float q = full > 1e-6f ? clampf( low / full, 0.f, 1.f ) : 0.f;
    column( cv, (int)x, yc - hf, yc + hf, mixc( dc, ui->c[C_HIGH], .30f + .55f * (1.f - q) ), a * .85f );
    column( cv, (int)x, yc - hl, yc + hl, dc, a );
}

static const char *kind_name( int kind )
{
    return kind == DJ_BLEND ? "BLEND" : kind == DJ_ECHO ? "ECHO OUT" : kind == DJ_FADE ? "FADE"
         : kind == DJ_START ? "START" : "";
}

/* the transition on the record it takes out, from where it begins */
static void band( dj_ui_t *ui, canvas_t *cv, int d, double pos, double spp, float xc, box_t z, bool label )
{
    const dj_status_t *st = &ui->st;
    if( st->from != d || st->kind == DJ_NONE || st->span_s <= 0. ) return;
    float x0 = xc + (float)((st->at_s - pos) / spp), x1 = xc + (float)((st->at_s + st->span_s - pos) / spp);
    uint32_t a = ui->c[C_ACCENT];
    rect( cv, x0, z.y, x1, z.y + z.h, a, .14f );
    rect( cv, x0 - ui->s, z.y, x0 + ui->s, z.y + z.h, a, .9f );
    rect( cv, x1 - ui->s / 2, z.y, x1 + ui->s / 2, z.y + z.h, a, .5f );
    if( !label ) return;
    char t[32];
    if( st->kind == DJ_BLEND ) snprintf( t, sizeof t, "BLEND %d", st->beats );
    else snprintf( t, sizeof t, "%s", kind_name( st->kind ) );
    text( ui, cv, F_CAPS, t, (int)x0 + S( ui, 7 ), z.y + S( ui, 6 ), S( ui, 120 ), a, AL_LEFT );
}

/* every beat; the half bars brighter, a bar's first beat marked in the deck's colour */
static void grid( dj_ui_t *ui, canvas_t *cv, int d, double pos, double spp, float xc, box_t z )
{
    const dj_grid_t *g = &ui->st.deck[d].grid;
    if( g->period <= 0. ) return;
    double P = g->period / DJ_RATE, ph = g->phase / DJ_RATE, half = (z.w / 2. + 4.) * spp;
    int64_t k = (int64_t)ceil( (pos - half - ph) / P ), k1 = (int64_t)floor( (pos + half - ph) / P );
    if( k1 - k > 600 ) return;
    uint32_t dc = ui->c[d ? C_B : C_A], t = ui->c[C_TEXT];
    float s = ui->s;
    for( ; k <= k1; k++ )
    {
        float x = xc + (float)((ph + k * P - pos) / spp);
        int m4 = (int)(((k - g->one + ui->follow[d].bar) % 4 + 4) % 4);
        rect( cv, x - .5f, z.y, x + .5f, z.y + z.h, t, m4 == 0 ? .30f : m4 == 2 ? .17f : .08f );
        if( m4 == 0 ) tri( cv, x - 4.f * s, z.y, x + 4.f * s, z.y, x, z.y + 5.f * s, dc, .95f );
    }
}

/* one record past the playhead; the same seconds a pixel for both decks, their own tempo applied */
static void zoom_draw( dj_ui_t *ui, canvas_t *cv, int d )
{
    box_t z = ui->zoom[d];
    const dj_deck_info_t *in = &ui->st.deck[d];
    const uint32_t *c = ui->c;
    uint32_t dc = c[d ? C_B : C_A];
    float s = ui->s, xc = z.x + z.w / 2.f, yc = z.y + z.h / 2.f, half = z.h / 2.f - 3.f * s;
    clip( cv, z );
    if( in->state != DJ_EMPTY )
    {
        double rate = in->rate > 0. ? in->rate : 1., spp = ui->zoom_s / z.w * rate, pos = ui->follow[d].pos;
        int n = z.w + 2;
        int64_t k0 = (int64_t)floor( pos / spp ) - z.w / 2 - 1;
        band( ui, cv, d, pos, spp, xc, z, true );
        grid( ui, cv, d, pos, spp, xc, z );
        if( wave_buf( ui, n ) )
        {
            dj_mix_Wave( ui->mix, d, k0 * spp, (k0 + n) * spp, n, ui->wl, ui->wf );
            float gain = powf( 10.f, in->trim_db / 20.f );
            int x0 = (int)floor( xc + (double)k0 - pos / spp + .5 );
            for( int i = 0; i < n; i++ )
                wave_col( ui, cv, d, (float)(x0 + i), yc, half, ui->wl[i], ui->wf[i], gain,
                          x0 + i < xc ? .4f : 1.f );
        }
    }
    const char *idle = in->state == DJ_EMPTY ? "EMPTY" : in->state == DJ_LOADING ? "LOADING" : NULL;
    if( idle )
        text( ui, cv, F_CAPS, idle, (int)xc + S( ui, 14 ), (int)(yc - ui->font_h[F_CAPS] / 2.f), S( ui, 120 ),
              c[C_DIM], AL_LEFT );
    rect( cv, xc - 4.f * s, z.y, xc + 4.f * s, z.y + z.h, c[C_TEXT], .05f );
    rect( cv, xc - s, z.y, xc + s, z.y + z.h, c[C_TEXT], .95f );
    tri( cv, xc - 6.f * s, z.y, xc + 6.f * s, z.y, xc, z.y + 7.f * s, c[C_TEXT], 1.f );
    tri( cv, xc - 6.f * s, z.y + z.h, xc + 6.f * s, z.y + z.h, xc, z.y + z.h - 7.f * s, c[C_TEXT], 1.f );
    bool on = in->gain > 0.f;
    rrect( cv, z.x + 8.f * s, z.y + 8.f * s, 22.f * s, 20.f * s, 5.f * s, 0, dc, on ? 1.f : .22f );
    text( ui, cv, F_BTN, d ? "B" : "A", z.x + S( ui, 8 ), z.y + S( ui, 8 ) + (S( ui, 20 ) - ui->font_h[F_BTN]) / 2,
          S( ui, 22 ), on ? c[C_BG] : dc, AL_CENTER );
    unclip( cv );
}

/* the whole record; a station, its last quarter hour and what is read ahead */
static void overview_draw( dj_ui_t *ui, canvas_t *cv, int d )
{
    box_t o = ui->ov[d];
    const dj_deck_info_t *in = &ui->st.deck[d];
    const uint32_t *c = ui->c;
    float s = ui->s;
    if( in->state == DJ_EMPTY ) return;
    clip( cv, o );
    double pos = ui->follow[d].pos, t0 = 0., t1 = in->length_s;
    if( in->t.endless || t1 <= 0. )
    {
        t1 = fmax( pos + in->ahead_s + 20., 900. );
        t0 = t1 - 900.;
    }
    int n = o.w;
    double spp = (t1 - t0) / n;
    float xp = o.x + (float)((pos - t0) / spp), yc = o.y + o.h / 2.f;
    band( ui, cv, d, pos, spp, xp, o, false );
    if( wave_buf( ui, n ) )
    {
        dj_mix_Wave( ui->mix, d, t0, t1, n, ui->wl, ui->wf );
        float gain = powf( 10.f, (in->trim_db + 9.f) / 20.f );   /* a tenth of a second's RMS sits 9 dB under the peaks */
        for( int i = 0; i < n; i++ )
            wave_col( ui, cv, d, (float)(o.x + i), yc, o.h / 2.f - 2.f * s, ui->wl[i], ui->wf[i], gain,
                      o.x + i < xp ? .45f : .9f );
    }
    float xr = o.x + (float)((pos + in->ahead_s - t0) / spp);
    if( xr < o.x + o.w ) rect( cv, xr, yc - .5f, o.x + o.w, yc + .5f, c[C_FAINT], .9f );
    rect( cv, xp - s, o.y, xp + s, o.y + o.h, c[C_TEXT], 1.f );
    unclip( cv );
}

/* how far apart the two grids' beats fall, in the mix's milliseconds, while both decks sound */
static bool sync_ms( const dj_ui_t *ui, double *ms )
{
    const dj_deck_info_t *a = &ui->st.deck[0], *b = &ui->st.deck[1];
    if( a->gain <= 0.f || b->gain <= 0.f || a->grid.period <= 0. || b->grid.period <= 0. ) return false;
    double fa = (a->pos_s * DJ_RATE - a->grid.phase) / a->grid.period;
    double fb = (b->pos_s * DJ_RATE - b->grid.phase) / b->grid.period;
    double d = (fa - floor( fa )) - (fb - floor( fb ));
    d -= floor( d + .5 );
    *ms = d * a->grid.period / DJ_RATE / (a->rate > 0. ? a->rate : 1.) * 1000.;
    return true;
}

/* the transition in hand, where the two records meet */
static void pill_draw( dj_ui_t *ui, canvas_t *cv )
{
    const dj_status_t *st = &ui->st;
    const uint32_t *c = ui->c;
    float s = ui->s;
    float xc = ui->zoom[0].x + ui->zoom[0].w / 2.f, yc = (ui->zoom[0].y + ui->zoom[0].h + ui->zoom[1].y) / 2.f;
    char t[128];
    if( st->kind != DJ_NONE )
    {
        const char *k = kind_name( st->kind );
        if( st->running && (st->kind == DJ_BLEND || st->kind == DJ_ECHO) && st->beats > 0 )
            snprintf( t, sizeof t, "%s   BEAT %d / %d", k, __MIN( (int)(st->progress * st->beats) + 1, st->beats ),
                      st->beats );
        else if( st->running )
            snprintf( t, sizeof t, "%s   %d%%", k, (int)(st->progress * 100.) );
        else
        {
            int left = (int)ceil( fmax( st->starts_in_s, 0. ) );
            if( st->kind == DJ_BLEND ) snprintf( t, sizeof t, "%s %d   IN %d:%02d", k, st->beats, left / 60, left % 60 );
            else snprintf( t, sizeof t, "%s   IN %d:%02d", k, left / 60, left % 60 );
        }
        int w = text_w( ui, cv, F_CAPS, t ) + S( ui, 32 ), h = S( ui, 24 );
        float x = xc - w / 2.f, y = yc - h / 2.f;
        shadow( cv, x, y, w, h, h / 2.f, 8.f * s, .6f );
        rrect( cv, x, y, w, h, h / 2.f, 0, c[C_BG], 1.f );
        if( st->running ) rrect( cv, x, y, fmaxf( (float)h, w * (float)st->progress ), h, h / 2.f, 0, c[C_ACCENT], .38f );
        rrect( cv, x, y, w, h, h / 2.f, 1.5f * s, c[C_ACCENT], .95f );
        text( ui, cv, F_CAPS, t, (int)x, (int)(yc - ui->font_h[F_CAPS] / 2.f), w, st->running ? c[C_TEXT] : c[C_ACCENT],
              AL_CENTER );
        xc += w / 2.f + 8.f * s;
    }
    double ms;
    if( !sync_ms( ui, &ms ) ) return;
    snprintf( t, sizeof t, "SYNC %+.1f ms", ms );
    uint32_t sc = fabs( ms ) <= 5. ? c[C_GOOD] : c[C_WARN];
    int w = text_w( ui, cv, F_CAPS, t ) + S( ui, 24 ), h = S( ui, 24 );
    float x = st->kind != DJ_NONE ? xc : xc - w / 2.f, y = yc - h / 2.f;
    rrect( cv, x, y, w, h, h / 2.f, 0, c[C_BG], 1.f );
    rrect( cv, x, y, w, h, h / 2.f, 1.5f * s, sc, .9f );
    text( ui, cv, F_CAPS, t, (int)x, (int)(yc - ui->font_h[F_CAPS] / 2.f), w, sc, AL_CENTER );
}

static void clock_str( char *b, size_t n, double s )
{
    long t = s > 0. ? (long)s : 0;
    if( t >= 3600 ) snprintf( b, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60 );
    else snprintf( b, n, "%ld:%02ld", t / 60, t % 60 );
}

/*****************************************************************************
 * The decks and the mixer
 *****************************************************************************/
static void deck_numbers( dj_ui_t *, canvas_t *, int, int, int, char *, size_t );

static void deck_draw( dj_ui_t *ui, canvas_t *cv, int d )
{
    box_t b = ui->deck[d];
    const dj_status_t *st = &ui->st;
    const dj_deck_info_t *in = &st->deck[d];
    const uint32_t *c = ui->c;
    uint32_t dc = c[d ? C_B : C_A];
    float s = ui->s;
    int x = b.x + S( ui, 16 ), y = b.y + S( ui, 14 ), w = b.w - S( ui, 32 ), bs = S( ui, 34 );
    bool on = in->gain > 0.f;
    if( on )
    {
        glow( cv, x + bs / 2.f, y + bs / 2.f, bs * 1.1f, dc, .22f );
        rrect( cv, x, y, bs, bs, 8.f * s, 0, dc, 1.f );
    }
    else rrect( cv, x, y, bs, bs, 8.f * s, 1.5f * s, dc, .8f );
    text( ui, cv, F_DECK, d ? "B" : "A", x, y + (bs - ui->font_h[F_DECK]) / 2, bs, on ? c[C_BG] : dc, AL_CENTER );

    const char *what = in->state == DJ_LOADING ? "LOADING" : in->state == DJ_CUED ? "CUED"
                     : in->state == DJ_LIVE ? "LIVE" : in->state == DJ_DONE ? "DONE"
                     : in->state == DJ_FAILED ? "FAILED" : "EMPTY";
    if( st->running && st->to == d ) what = "MIXING IN";
    else if( st->running && st->from == d ) what = "MIXING OUT";
    else if( st->armed && st->to == d ) what = "CUED NEXT";
    uint32_t wc = in->state == DJ_FAILED ? c[C_BAD] : on ? dc : in->state == DJ_CUED ? c[C_ACCENT] : c[C_DIM];
    int pw = text_w( ui, cv, F_CAPS, what ) + S( ui, 20 ), ph = S( ui, 20 );
    int px = x + bs + S( ui, 10 ), py = y + (bs - ph) / 2;
    rrect( cv, px, py, pw, ph, ph / 2.f, 0, wc, .16f );
    text( ui, cv, F_CAPS, what, px, py + (ph - ui->font_h[F_CAPS]) / 2, pw, wc, AL_CENTER );

    char t[96] = "";
    if( in->codec[0] )   /* the source, as it arrives */
    {
        if( in->lossless && in->src_rate )
            snprintf( t, sizeof t, "%s  %.1f kHz  %u-bit", in->codec, in->src_rate / 1000., in->src_bits );
        else if( in->kbps > 0 ) snprintf( t, sizeof t, "%s  %d kbps", in->codec, in->kbps );
        else snprintf( t, sizeof t, "%s", in->codec );
        uint32_t sc = in->lossless ? c[C_GOOD] : c[C_DIM];
        int tw = __MIN( text_w( ui, cv, F_SMALL, t ), x + w - px - pw - S( ui, 24 ) );
        text( ui, cv, F_SMALL, t, x + w - tw, y + (bs - ui->font_h[F_SMALL]) / 2, tw, sc, AL_LEFT );
        if( in->lossless )
        {
            float gx = x + w - tw - 10.f * s, gy = y + bs / 2.f;
            tri( cv, gx - 4.f * s, gy, gx + 4.f * s, gy, gx, gy + 5.f * s, sc, 1.f );
            tri( cv, gx - 4.f * s, gy, gx, gy - 3.f * s, gx + 4.f * s, gy, sc, .7f );
        }
    }
    y += bs + S( ui, 10 );
    if( in->state == DJ_EMPTY )
    {
        text( ui, cv, F_BODY, "Waiting for the next track", x, y + S( ui, 4 ), w, c[C_FAINT], AL_LEFT );
        return;
    }
    bool station = in->now_playing && *in->now_playing;
    const char *title = station ? in->now_playing : in->t.title ? in->t.title : "?";
    const char *artist = station ? in->t.title : in->t.artist;
    text( ui, cv, F_TITLE, title, x, y, w, c[C_TEXT], AL_LEFT );
    y += ui->font_h[F_TITLE];
    if( artist ) text( ui, cv, F_BODY, artist, x, y, w, c[C_DIM], AL_LEFT );
    y += ui->font_h[F_BODY] + S( ui, 2 );
    if( in->t.label ) text( ui, cv, F_SMALL, in->t.label, x, y, w, mixc( dc, c[C_DIM], .4f ), AL_LEFT );
    deck_numbers( ui, cv, d, x, w, t, sizeof t );
}

/* tempo, pitch and time; the bar's four lamps and how well the grid sits */
static void deck_numbers( dj_ui_t *ui, canvas_t *cv, int d, int x, int w, char *t, size_t tn )
{
    box_t b = ui->deck[d];
    const dj_deck_info_t *in = &ui->st.deck[d];
    const uint32_t *c = ui->c;
    float s = ui->s;
    bool on = in->gain > 0.f;
    int ny = b.y + b.h - S( ui, 88 );
    double pos = ui->follow[d].pos;
    if( in->grid.period > 0. )
    {
        snprintf( t, tn, "%.1f", in->grid.bpm * in->rate );
        int bw = num( ui, cv, F_XL, t, x, ny, c[C_TEXT], AL_LEFT );
        text( ui, cv, F_CAPS, "BPM", x + bw + S( ui, 8 ), ny + S( ui, 9 ), S( ui, 60 ), c[C_DIM], AL_LEFT );
        double pitch = (in->rate - 1.) * 100.;
        snprintf( t, tn, "%+.2f%%", pitch );
        num( ui, cv, F_M, t, x + bw + S( ui, 8 ), ny + S( ui, 24 ), fabs( pitch ) < .005 ? c[C_DIM] : c[C_ACCENT],
             AL_LEFT );
    }
    else
    {
        num( ui, cv, F_XL, "--", x, ny, c[C_FAINT], AL_LEFT );
        text( ui, cv, F_CAPS, in->state == DJ_LOADING ? "READING" : "NO STEADY BEAT", x + S( ui, 52 ),
              ny + S( ui, 16 ), S( ui, 140 ), c[C_DIM], AL_LEFT );
    }
    clock_str( t, tn, pos );
    num( ui, cv, F_L, t, x + w, ny + S( ui, 2 ), c[C_TEXT], AL_RIGHT );
    if( in->t.endless || in->length_s <= 0. )
        text( ui, cv, F_CAPS, "LIVE STREAM", x + w - S( ui, 140 ), ny + S( ui, 40 ), S( ui, 140 ), c[C_DIM], AL_RIGHT );
    else
    {
        double left = in->length_s - pos;
        t[0] = '-';
        clock_str( t + 1, tn - 1, left );
        num( ui, cv, F_M, t, x + w, ny + S( ui, 36 ), on && left < 30. ? c[C_BAD] : c[C_DIM], AL_RIGHT );
    }
    if( in->state < DJ_CUED ) return;
    int ly = b.y + b.h - S( ui, 26 );
    float lw = 22.f * s, lh = 7.f * s, lg = 5.f * s;
    if( in->grid.period > 0. )
        snprintf( t, tn, "GRID %d%%  %.1f ms   %.1f LUFS  TRIM %+.1f",
                  (int)lroundf( in->grid.conf * 100.f ), in->grid.jitter, in->loud, in->trim_db );
    else snprintf( t, tn, "%.1f LUFS  TRIM %+.1f", in->loud, in->trim_db );
    int ix = x + (int)(4.f * (lw + lg) + 8.f * s);
    text( ui, cv, F_SMALL, t, ix, ly + (int)(lh / 2.f) - ui->font_h[F_SMALL] / 2, x + w - ix, c[C_DIM], AL_RIGHT );
}

/* the bar: four lamps, the beat on lit */
static void deck_lamps( dj_ui_t *ui, canvas_t *cv, int d )
{
    box_t b = ui->deck[d];
    const dj_deck_info_t *in = &ui->st.deck[d];
    const uint32_t *c = ui->c;
    uint32_t dc = c[d ? C_B : C_A];
    float s = ui->s, lw = 22.f * s, lh = 7.f * s, lg = 5.f * s;
    if( in->state < DJ_CUED || in->grid.period <= 0. ) return;
    int x = b.x + S( ui, 16 ), ly = b.y + b.h - S( ui, 26 );
    bool on = in->gain > 0.f;
    double beat = (ui->follow[d].pos * DJ_RATE - in->grid.phase) / in->grid.period;
    int64_t k = (int64_t)floor( beat );
    int m4 = (int)(((k - in->grid.one + ui->follow[d].bar) % 4 + 4) % 4);
    float frac = (float)(beat - floor( beat ));
    for( int i = 0; i < 4; i++ )
    {
        float lx = x + i * (lw + lg);
        bool lit = i == m4 && on;
        rrect( cv, lx, ly, lw, lh, lh / 2.f, 0, i == 0 ? dc : mixc( dc, c[C_TEXT], .3f ), lit ? 1.f - .6f * frac : .13f );
        if( lit && frac < .25f ) glow( cv, lx + lw / 2.f, ly + lh / 2.f, lw, dc, .3f * (1.f - 4.f * frac) );
    }
}

static void lamp( dj_ui_t *ui, canvas_t *cv, float cx, float cy, const char *label, bool lit, uint32_t col )
{
    float w = 52.f * ui->s, h = 18.f * ui->s;
    if( lit )
    {
        glow( cv, cx, cy, w * .8f, col, .3f );
        rrect( cv, cx - w / 2.f, cy - h / 2.f, w, h, h / 2.f, 0, col, 1.f );
    }
    else rrect( cv, cx - w / 2.f, cy - h / 2.f, w, h, h / 2.f, ui->s, ui->c[C_FAINT], .9f );
    text( ui, cv, F_CAPS, label, (int)(cx - w / 2.f), (int)(cy - ui->font_h[F_CAPS] / 2.f), (int)w,
          lit ? ui->c[C_BG] : ui->c[C_FAINT], AL_CENTER );
}

static void cap( dj_ui_t *ui, canvas_t *cv, float x, float y, float w, float h )
{
    float s = ui->s;
    shadow( cv, x, y, w, h, 3.f * s, 5.f * s, .6f );
    rrect2( cv, x, y, w, h, 3.f * s, 0, mixc( ui->c[C_RAISED], ui->c[C_TEXT], .35f ),
            mixc( ui->c[C_RAISED], ui->c[C_TEXT], .14f ), 1.f );
}

static void mixer_draw( dj_ui_t *ui, canvas_t *cv )
{
    const dj_status_t *st = &ui->st;
    const uint32_t *c = ui->c;
    box_t m = ui->mixer;
    float s = ui->s;
    for( int d = 0; d < DJ_DECKS; d++ )
    {
        const dj_deck_info_t *in = &st->deck[d];
        uint32_t dc = c[d ? C_B : C_A];
        float kx, ky, fx, top, bot, mx;
        strip_geom( ui, d, &kx, &ky, &fx, &top, &bot, &mx );
        /* the bass EQ: noon is flat, seven o'clock the kill */
        float v = clampf( in->low_db / -26.f, 0.f, 1.f ), ang = -2.356f * v, r = 15.f * s;
        if( v > .01f ) arc( cv, kx, ky, 19.f * s, 3.f * s, ang, 0.f, dc, 1.f );
        disc( cv, kx, ky, r, mixc( c[C_RAISED], c[C_TEXT], .10f ), 1.f );
        arc( cv, kx, ky, r - .75f * s, 1.5f * s, -4.f, 4.f, c[C_EDGE], 1.f );
        line( cv, kx + sinf( ang ) * r * .3f, ky - cosf( ang ) * r * .3f, kx + sinf( ang ) * r * .8f,
              ky - cosf( ang ) * r * .8f, 2.5f * s, v > .01f ? dc : c[C_TEXT], 1.f );
        float g = clampf( in->gain, 0.f, 1.f ), fy = bot - g * (bot - top);
        cap( ui, cv, fx - 15.f * s, fy - 7.f * s, 30.f * s, 14.f * s );
        rect( cv, fx - 12.f * s, fy - .75f * s, fx + 12.f * s, fy + .75f * s, g > 0.f ? dc : c[C_DIM], 1.f );
        meter( ui, cv, mx, top, bot, 10.f * s, ui->meter[d].db, ui->meter[d].peak, true );
    }
    float cx = m.x + m.w / 2.f, top = m.y + 92.f * s, bot = m.y + 172.f * s;
    float gr = clampf( -st->gr_db / 12.f, 0.f, 1.f );
    if( gr > .002f ) rrect( cv, cx - 22.f * s, top, 8.f * s, (bot - top) * gr, 2.f * s, 0, c[C_BAD], 1.f );
    meter( ui, cv, cx + 14.f * s, top, bot, 8.f * s, ui->out.db, ui->out.peak, true );
    /* the crossfader stands where the two channel faders' gains put it */
    float y = m.y + 198.f * s, x0 = m.x + 74.f * s, x1 = m.x + m.w - 74.f * s;
    float xf = x0 + (x1 - x0) * (st->xfade + 1.f) / 2.f;
    cap( ui, cv, xf - 9.f * s, y - 13.f * s, 18.f * s, 26.f * s );
    rect( cv, xf - .75f * s, y - 10.f * s, xf + .75f * s, y + 10.f * s,
          mixc( c[C_A], c[C_B], (st->xfade + 1.f) / 2.f ), 1.f );
}

static void mixer_lamps( dj_ui_t *ui, canvas_t *cv )
{
    const dj_status_t *st = &ui->st;
    float cx = ui->mixer.x + ui->mixer.w / 2.f, s = ui->s;
    lamp( ui, cv, cx, ui->mixer.y + 40.f * s, "ECHO", st->kind == DJ_ECHO && st->running, ui->c[C_ACCENT] );
    lamp( ui, cv, cx, ui->mixer.y + 62.f * s, "DUCK", st->master_db < -.5f, ui->c[C_WARN] );
}

/*****************************************************************************
 * The header, the crate, the moods, the request box, the status line
 *****************************************************************************/
static void head_draw( dj_ui_t *ui, canvas_t *cv, float dt, bool fast )
{
    const dj_status_t *st = &ui->st;
    const dj_brain_t *br = &ui->br;
    const uint32_t *c = ui->c;
    box_t h = ui->head;
    float s = ui->s, cy = h.y + h.h / 2.f;
    bool playing = st->attached && !st->paused;
    if( fast && playing ) ui->spin = fmodf( ui->spin + dt * 2.f * PI_F * 33.333f / 60.f, 2.f * PI_F );
    if( fast ) vinyl( cv, 28.f * s, cy, 16.f * s, ui->spin, c );

    const char *lab = !br->on_air ? "OFF AIR" : playing ? "ON AIR" : st->paused ? "PAUSED" : "STANDBY";
    int x = S( ui, 148 ), bh = S( ui, 26 ), bw = text_w( ui, cv, F_CAPS, lab ) + S( ui, 36 );
    box_t air = { x, (int)(cy - bh / 2.f), bw, bh };
    bool hover = ui->hover_id == B_AIR, lit = br->on_air && playing;
    uint32_t lc = br->on_air ? c[C_AIR] : c[C_DIM];
    if( !fast ) hot( ui, air, B_AIR, 0 );
    else if( lit )
    {
        float pulse = .7f + .3f * sinf( (float)(mdate() % 2000000) / 2000000.f * 2.f * PI_F );
        glow( cv, air.x + bw / 2.f, cy, bw * .75f, c[C_AIR], .28f * pulse );
        rrect( cv, air.x, air.y, bw, bh, bh / 2.f, 0, mixc( c[C_AIR], 0xffffff, hover ? .14f : 0.f ), 1.f );
        disc( cv, air.x + 14.f * s, cy, 3.5f * s, 0xffffff, .6f + .4f * pulse );
    }
    else
    {
        if( hover ) rrect( cv, air.x, air.y, bw, bh, bh / 2.f, 0, lc, .12f );
        rrect( cv, air.x, air.y, bw, bh, bh / 2.f, 1.5f * s, lc, .8f );
        disc( cv, air.x + 14.f * s, cy, 3.5f * s, lc, .8f );
    }
    if( fast )
        text( ui, cv, F_CAPS, lab, air.x + S( ui, 24 ), (int)(cy - ui->font_h[F_CAPS] / 2.f), bw - S( ui, 28 ),
              lit ? 0xffffff : lc, AL_LEFT );
    x += bw + S( ui, 12 );
    if( fast && br->busy )   /* the DJ at work */
    {
        float a = (float)(mdate() % 1000000) / 1000000.f * 2.f * PI_F;
        arc( cv, x + 8.f * s, cy, 7.f * s, 2.f * s, a, a + 4.f, c[C_ACCENT], 1.f );
    }
    x += S( ui, 28 );
    if( fast ) return;

    /* from the right: the clock, the theme, the voice, the volume */
    struct tm tm;
    time_t now = time( NULL );
    localtime_s( &tm, &now );
    char t[200];
    snprintf( t, sizeof t, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec );
    int right = h.w - S( ui, 16 ), by = (int)(cy - S( ui, 13 )), bb = S( ui, 26 );
    right -= num( ui, cv, F_M, t, right, (int)(cy - ui->font_h[F_M] / 2.f), c[C_TEXT], AL_RIGHT ) + S( ui, 18 );
    snprintf( t, sizeof t, "%s", presets[ui->preset].name );
    for( char *p = t; *p; p++ ) if( *p >= 'a' && *p <= 'z' ) *p -= 'a' - 'A';
    int tw = text_w( ui, cv, F_BTN, t ) + S( ui, 46 );
    button( ui, cv, (box_t){ right - tw, by, tw, bb }, t, 0xE790, B_THEME, 0, ST_OUTLINE, c[C_DIM] );
    right -= tw + S( ui, 8 );
    const char *vo = br->voice == 2 ? "VOICE ALL" : br->voice == 1 ? "VOICE BLOCKS" : "VOICE OFF";
    tw = text_w( ui, cv, F_BTN, vo ) + S( ui, 46 );
    button( ui, cv, (box_t){ right - tw, by, tw, bb }, vo, 0xE720, B_VOICE, 0, ST_OUTLINE,
            br->voice ? c[C_ACCENT] : c[C_DIM] );
    right -= tw + S( ui, 16 );
    button( ui, cv, (box_t){ right - bb, by, bb, bb }, "", 0xE710, B_VOLUP, 0, ST_OUTLINE, c[C_DIM] );
    right -= bb;
    snprintf( t, sizeof t, "%d", br->volume );
    num( ui, cv, F_M, t, right - S( ui, 22 ), (int)(cy - ui->font_h[F_M] / 2.f), c[C_TEXT], AL_CENTER );
    right -= S( ui, 44 );
    button( ui, cv, (box_t){ right - bb, by, bb, bb }, "", 0xE738, B_VOLDN, 0, ST_OUTLINE, c[C_DIM] );
    right -= bb + S( ui, 4 );
    glyph( ui, cv, 0xE767, right - 10.f * s, cy, c[C_DIM] );
    right -= S( ui, 30 );

    /* the programme, between */
    int pw = right - x;
    if( pw < S( ui, 80 ) || !br->block[0] ) return;
    int bw2 = __MIN( text_w( ui, cv, F_BODY, br->block ), pw );
    int y1 = (int)cy - ui->font_h[F_BODY] + S( ui, 1 ), y2 = (int)cy + S( ui, 2 );
    text( ui, cv, F_BODY, br->block, x, y1, bw2, c[C_TEXT], AL_LEFT );
    if( br->pick[0] && pw - bw2 > S( ui, 40 ) )
        text( ui, cv, F_BODY, br->pick, x + bw2 + S( ui, 10 ), y1, pw - bw2 - S( ui, 10 ), mixc( c[C_ACCENT], c[C_TEXT], .3f ),
              AL_LEFT );
    if( br->next_block[0] )
    {
        snprintf( t, sizeof t, "NEXT  %s  %s", br->next_block, br->next_at );
        text( ui, cv, F_SMALL, t, x, y2, pw, c[C_DIM], AL_LEFT );
    }
}

static void next_draw( dj_ui_t *ui, canvas_t *cv )
{
    box_t b = ui->next;
    const dj_brain_t *br = &ui->br;
    const uint32_t *c = ui->c;
    int x = b.x + S( ui, 14 ), y = b.y + S( ui, 36 ), w = b.w - S( ui, 28 ), rh = S( ui, 19 );
    if( br->n_next == 0 )
    {
        text( ui, cv, F_SMALL, br->on_air ? "The DJ picks the next one as it goes." : "Off air.", x, y, w,
              c[C_FAINT], AL_LEFT );
        return;
    }
    for( int i = 0; i < br->n_next && y + rh <= b.y + b.h - S( ui, 6 ); i++, y += rh )
    {
        char n[8];
        snprintf( n, sizeof n, "%d", i + 1 );
        num( ui, cv, F_S, n, x + S( ui, 7 ), y + (ui->font_h[F_BODY] - ui->font_h[F_S]) / 2, c[C_FAINT], AL_CENTER );
        text( ui, cv, F_BODY, br->next[i], x + S( ui, 22 ), y, w - S( ui, 22 ), i ? c[C_DIM] : c[C_TEXT], AL_LEFT );
    }
}

/* the programme's moods, deck A's colour to deck B's */
static void moods_draw( dj_ui_t *ui, canvas_t *cv )
{
    box_t b = ui->moods;
    const dj_brain_t *br = &ui->br;
    int x0 = b.x + S( ui, 14 ), x = x0, y = b.y + S( ui, 34 ), right = b.x + b.w - S( ui, 14 ), ph = S( ui, 24 );
    if( br->n_moods == 0 )
        text( ui, cv, F_SMALL, "No moods in the programme.", x0, y, right - x0, ui->c[C_FAINT], AL_LEFT );
    for( int i = 0; i < br->n_moods; i++ )
    {
        int w = __MIN( text_w( ui, cv, F_BTN, br->moods[i] ) + S( ui, 24 ), right - x0 );
        if( x + w > right )
        {
            x = x0;
            y += ph + S( ui, 6 );
        }
        if( y + ph > b.y + b.h - S( ui, 8 ) ) break;
        float t = br->n_moods > 1 ? (float)i / (br->n_moods - 1) : 0.f;
        button( ui, cv, (box_t){ x, y, w, ph }, br->moods[i], 0, B_MOOD, i, ST_OUTLINE,
                mixc( ui->c[C_A], ui->c[C_B], t ) );
        x += w + S( ui, 6 );
    }
}

static void req_draw( dj_ui_t *ui, canvas_t *cv )
{
    box_t f = ui->field;
    const uint32_t *c = ui->c;
    float s = ui->s;
    rrect( cv, f.x, f.y, f.w, f.h, 8.f * s, 0, c[C_BG], 1.f );
    rrect( cv, f.x, f.y, f.w, f.h, 8.f * s, ui->edit_focus ? 1.5f * s : s, ui->edit_focus ? c[C_ACCENT] : c[C_EDGE], 1.f );
    int gap = S( ui, 8 ), bh = S( ui, 28 ), half = (f.w - gap) / 2, y = f.y + f.h + S( ui, 10 );
    button( ui, cv, (box_t){ f.x, y, half, bh }, "PLAY NOW", 0xE768, B_PLAY, 0, ST_FILL, c[C_ACCENT] );
    button( ui, cv, (box_t){ f.x + half + gap, y, f.w - half - gap, bh }, "QUEUE", 0xE710, B_QUEUE, 0, ST_OUTLINE,
            c[C_ACCENT] );
    y += bh + S( ui, 8 );
    button( ui, cv, (box_t){ f.x, y, half, bh }, "MIX NEXT NOW", 0xE893, B_NEXT, 0, ST_OUTLINE, c[C_DIM] );
    button( ui, cv, (box_t){ f.x + half + gap, y, f.w - half - gap, bh }, "DJ'S CHOICE", 0xE8B1, B_CHOOSE, 0,
            ST_OUTLINE, c[C_DIM] );
}

/* what went wrong, else what the DJ said, else its last plan; the mix on the right */
static void status_draw( dj_ui_t *ui, canvas_t *cv )
{
    static const char lq[] = { (char)0xe2, (char)0x80, (char)0x9c, 0 }, rq[] = { (char)0xe2, (char)0x80, (char)0x9d, 0 };
    const dj_status_t *st = &ui->st;
    const uint32_t *c = ui->c;
    box_t b = ui->status;
    int y = b.y + (b.h - ui->font_h[F_SMALL]) / 2, x = S( ui, 14 ), right = b.w - S( ui, 14 );
    char t[400], mixed[32];
    clock_str( mixed, sizeof mixed, st->mixed_s );
    snprintf( t, sizeof t, "MIXED %s     48 kHz FLOAT     OUT %.1f dB     LIMITER %.1f dB", mixed,
              fmaxf( st->out_db, -99.f ), st->gr_db );
    int tw = text_w( ui, cv, F_SMALL, t );
    text( ui, cv, F_SMALL, t, right - tw, y, tw, c[C_DIM], AL_LEFT );
    int w = right - tw - S( ui, 30 ) - x, lw = S( ui, 48 );
    mtime_t now = mdate();
    const char *tag = NULL, *what = NULL;
    uint32_t col = c[C_DIM];
    if( ui->br.error[0] && now - ui->error_at < 20 * CLOCK_FREQ )
    {
        tag = "ERROR"; what = ui->br.error; col = c[C_BAD];
    }
    else if( ui->br.said[0] && now - ui->said_at < 15 * CLOCK_FREQ )
    {
        snprintf( t, sizeof t, "%s%s%s", lq, ui->br.said, rq );
        tag = "SAID"; what = t; col = c[C_TEXT];
    }
    else if( st->note[0] )
    {
        tag = "PLAN"; what = st->note;
    }
    if( tag == NULL ) return;
    text( ui, cv, F_CAPS, tag, x, b.y + (b.h - ui->font_h[F_CAPS]) / 2, lw, col == c[C_DIM] ? c[C_ACCENT] : col, AL_LEFT );
    text( ui, cv, F_SMALL, what, x + lw, y, w - lw, col, AL_LEFT );
}

/*****************************************************************************
 * Commands: the DJ's own verbs, queued for its thread
 *****************************************************************************/
static void order( dj_ui_t *ui, const char *fmt, ... )
{
    char buf[1536];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof buf, fmt, ap );
    va_end( ap );
    if( ui->command_cb ) ui->command_cb( ui->opaque, buf );
}

static void uri_enc( char *out, size_t cap, const char *in )
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for( const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++ )
    {
        unsigned ch = *p;
        if( (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr( "-_.~", (int)ch ) )
            out[o++] = (char)ch;
        else
        {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = 0;
}

static void request( dj_ui_t *ui, bool queue )
{
    wchar_t w[300];
    char u[900], e[480 * 3];
    if( ui->edit == NULL || GetWindowTextW( ui->edit, w, (int)COUNT( w ) ) <= 0 ) return;
    if( WideCharToMultiByte( CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL ) <= 0 ) return;
    char *p = u;
    while( *p == ' ' ) p++;
    for( char *q = p + strlen( p ); q > p && q[-1] == ' '; ) *--q = 0;
    if( !*p ) return;
    uri_enc( e, sizeof e, p );
    order( ui, "request?text=%s%s", e, queue ? "&queue=1" : "" );
    SetWindowTextW( ui->edit, L"" );
}

/*****************************************************************************
 * The window's look: fonts, the title bar, the icon
 *****************************************************************************/
static HICON make_icon( const uint32_t *c, int size )
{
    BITMAPINFO bi;
    memset( &bi, 0, sizeof bi );
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP col = CreateDIBSection( NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0 );
    if( col == NULL ) return NULL;
    uint32_t *px = bits;
    float R = size / 2.f - .5f;
    for( int y = 0; y < size; y++ )
        for( int x = 0; x < size; x++ )
        {
            float cov;
            uint32_t v = vinyl_px( c, x + .5f - size / 2.f, y + .5f - size / 2.f, R, -.6f, &cov );
            uint32_t a = (uint32_t)lroundf( cov * 255.f );
            px[y * size + x] = a ? a << 24 | v : 0;
        }
    BYTE *mbits = calloc( (size_t)((size + 15) / 16) * 2 * size, 1 );
    HBITMAP mask = mbits ? CreateBitmap( size, size, 1, 1, mbits ) : NULL;
    free( mbits );
    HICON icon = NULL;
    if( mask )
    {
        ICONINFO ii = { TRUE, 0, 0, mask, col };
        icon = CreateIconIndirect( &ii );
        DeleteObject( mask );
    }
    DeleteObject( col );
    return icon;
}

typedef HRESULT (WINAPI *dwm_attr_fn)( HWND, DWORD, LPCVOID, DWORD );

/* the theme over the whole window: the title bar where Windows lets it, the request box, the icon */
static void retheme( dj_ui_t *ui )
{
    fonts_make( ui );
    if( ui->edit ) SendMessageW( ui->edit, WM_SETFONT, (WPARAM)ui->font[F_BODY], TRUE );
    if( ui->edit_brush ) DeleteObject( ui->edit_brush );
    ui->edit_brush = CreateSolidBrush( cref( ui->c[C_BG] ) );
    if( ui->dwm == NULL ) ui->dwm = LoadLibraryW( L"dwmapi.dll" );
    dwm_attr_fn set = ui->dwm ? (dwm_attr_fn)(void *)GetProcAddress( ui->dwm, "DwmSetWindowAttribute" ) : NULL;
    if( set )
    {
        BOOL dark = TRUE;
        COLORREF caption = cref( ui->c[C_RAISED] ), txt = cref( ui->c[C_TEXT] ), edge = cref( ui->c[C_EDGE] );
        set( ui->hwnd, 20, &dark, sizeof dark );            /* DWMWA_USE_IMMERSIVE_DARK_MODE */
        set( ui->hwnd, 34, &edge, sizeof edge );            /* DWMWA_BORDER_COLOR, Windows 11 */
        set( ui->hwnd, 35, &caption, sizeof caption );      /* DWMWA_CAPTION_COLOR */
        set( ui->hwnd, 36, &txt, sizeof txt );              /* DWMWA_TEXT_COLOR */
    }
    HICON big = make_icon( ui->c, S( ui, 32 ) ), small = make_icon( ui->c, S( ui, 16 ) );
    if( big ) SendMessageW( ui->hwnd, WM_SETICON, ICON_BIG, (LPARAM)big );
    if( small ) SendMessageW( ui->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small );
    if( big && ui->icon_big ) DestroyIcon( ui->icon_big );
    if( small && ui->icon_small ) DestroyIcon( ui->icon_small );
    if( big ) ui->icon_big = big;
    if( small ) ui->icon_small = small;
    layout( ui );
    InvalidateRect( ui->hwnd, NULL, FALSE );
}

static void click( dj_ui_t *ui, int id, int arg )
{
    char e[48 * 3 + 1];
    mtime_t now = mdate();
    /* a double click is one press: a request twice would load the second over the first */
    if( id != B_VOLDN && id != B_VOLUP && id == ui->flash_id && arg == ui->flash_arg
     && now - ui->flash_at < 7 * CLOCK_FREQ / 10 )
        return;
    switch( id )
    {
        case B_AIR:    order( ui, "air?on=%d", ui->br.on_air ? 0 : 1 ); break;
        case B_VOICE:  order( ui, "air?voice=%s", ui->br.voice == 0 ? "blocks" : ui->br.voice == 1 ? "all" : "off" ); break;
        case B_VOLDN:  order( ui, "volume?level=-5" ); break;
        case B_VOLUP:  order( ui, "volume?level=%%2B5" ); break;
        case B_PLAY:   request( ui, false ); break;
        case B_QUEUE:  request( ui, true ); break;
        case B_NEXT:   order( ui, "next" ); break;
        case B_CHOOSE: order( ui, "choose" ); break;
        case B_MOOD:
            if( arg < 0 || arg >= ui->br.n_moods ) break;
            uri_enc( e, sizeof e, ui->br.moods[arg] );
            order( ui, "request?text=%s", e );
            break;
        case B_THEME:
        {
            int next = (ui->preset + 1) % N_PRESETS;
            theme_save( ui, next );
            theme_load( ui, next );
            retheme( ui );
            break;
        }
    }
    ui->flash_id = id;
    ui->flash_arg = arg;
    ui->flash_at = now;
    ui->mid_dirty = true;
}

/*****************************************************************************
 * A frame
 *****************************************************************************/
static void meter_feed( meter_t *m, float db, mtime_t now )
{
    if( !(db > -120.f) ) db = -120.f;
    m->db = db;
    if( db >= m->peak || now - m->peak_at > 3 * CLOCK_FREQ / 2 )
    {
        m->peak = db;
        m->peak_at = now;
    }
}

/* the beat of the bar, 0 to 3, nearest the playhead; with the count the deck took over, or its own */
static int bar_beat( const dj_ui_t *ui, int d, bool taken )
{
    const dj_grid_t *g = &ui->st.deck[d].grid;
    if( g->period <= 0. ) return -1;
    int64_t k = (int64_t)floor( (ui->st.deck[d].pos_s * DJ_RATE - g->phase) / g->period + .5 );
    return (int)(((k - g->one + (taken ? ui->follow[d].bar : 0)) % 4 + 4) % 4);
}

static float refresh( dj_ui_t *ui )
{
    mtime_t now = mdate();
    float dt = ui->last_frame ? (float)(now - ui->last_frame) / CLOCK_FREQ : 0.f;
    ui->last_frame = now;
    if( ui->have_st ) dj_mix_StatusClean( &ui->st );
    dj_mix_Status( ui->mix, &ui->st );
    ui->have_st = true;
    if( ui->brain_cb ) ui->brain_cb( ui->opaque, &ui->br );
    for( int d = 0; d < DJ_DECKS; d++ )
    {
        /* the mixer moves a deck a block at a time; the screen moves it every frame */
        const dj_deck_info_t *in = &ui->st.deck[d];
        follow_t *f = &ui->follow[d];
        uint32_t id = hash( in->t.url );
        bool moving = ui->st.attached && !ui->st.paused && in->gain > 0.f;
        double pred = f->pos + (moving ? dt * in->rate : 0.);
        if( f->track != id ) f->bar = 0;
        if( !f->ok || f->track != id || fabs( in->pos_s - pred ) > .3 ) pred = in->pos_s;
        else pred += .12 * (in->pos_s - pred);
        f->pos = pred;
        f->track = id;
        f->ok = in->state != DJ_EMPTY;
        meter_feed( &ui->meter[d], in->state != DJ_EMPTY && in->gain > 0.f ? in->level_db : -120.f, now );
    }
    meter_feed( &ui->out, ui->st.out_db, now );
    /* the analysis knows half bars, not bars: in a blend the incoming record takes over the
       outgoing one's count, as the DJ counting along would */
    const dj_status_t *st = &ui->st;
    if( st->running && (st->kind == DJ_BLEND || st->kind == DJ_ECHO) && st->from >= 0 && st->to >= 0 )
    {
        int a = bar_beat( ui, st->from, true ), b = bar_beat( ui, st->to, false );
        if( a >= 0 && b >= 0 ) ui->follow[st->to].bar = (a - b + 4) % 4;
    }
    if( strcmp( ui->br.error, ui->error_seen ) )
    {
        snprintf( ui->error_seen, sizeof ui->error_seen, "%s", ui->br.error );
        ui->error_at = now;
    }
    if( strcmp( ui->br.said, ui->said_seen ) )
    {
        snprintf( ui->said_seen, sizeof ui->said_seen, "%s", ui->br.said );
        ui->said_at = now;
    }
    return dt;
}

/* the live deck's track in the title bar and on the taskbar */
static void retitle( dj_ui_t *ui )
{
    int live = ui->st.live;
    char t[400] = "VLC DJ";
    if( live >= 0 && live < DJ_DECKS )
    {
        const dj_deck_info_t *in = &ui->st.deck[live];
        const char *title = in->now_playing && *in->now_playing ? in->now_playing : in->t.title;
        if( title && in->t.artist && !(in->now_playing && *in->now_playing) )
            snprintf( t, sizeof t, "%s - %s  |  VLC DJ", in->t.artist, title );
        else if( title ) snprintf( t, sizeof t, "%s  |  VLC DJ", title );
    }
    wchar_t w[300];
    if( utf16( t, w, (int)COUNT( w ) ) && wcscmp( w, ui->title ) )
    {
        wcscpy( ui->title, w );
        SetWindowTextW( ui->hwnd, w );
    }
}

static void resize( dj_ui_t *, int, int );

static void copy_box( canvas_t *dst, const canvas_t *src, box_t b )
{
    int x0 = __MAX( b.x, 0 ), y0 = __MAX( b.y, 0 ), x1 = __MIN( b.x + b.w, dst->w ), y1 = __MIN( b.y + b.h, dst->h );
    for( int y = y0; x1 > x0 && y < y1; y++ )
        memcpy( dst->px + (size_t)y * dst->w + x0, src->px + (size_t)y * src->w + x0, (size_t)(x1 - x0) * sizeof *dst->px );
}

/* the words and the buttons a few times a second, or when the pointer changes what they show;
   the records, the meters, the faders every frame over them */
static void frame( dj_ui_t *ui )
{
    canvas_t *cv = &ui->frame, *mid = &ui->mid;
    RECT cr;
    if( GetClientRect( ui->hwnd, &cr ) && (cr.right != cv->w || cr.bottom != cv->h) ) resize( ui, cr.right, cr.bottom );
    if( cv->px == NULL || mid->px == NULL || ui->chrome.px == NULL ) return;
    size_t bytes = (size_t)cv->w * cv->h * sizeof *cv->px;
    float dt = refresh( ui );
    mtime_t now = mdate();
    if( ui->chrome_dirty )
    {
        chrome( ui );
        ui->mid_dirty = true;
    }
    bool slow = ui->mid_dirty || now - ui->mid_at >= CLOCK_FREQ / 4;
    if( slow )
    {
        GdiFlush();
        memcpy( mid->px, ui->chrome.px, bytes );
        mid->gdi = false;
        unclip( mid );
        ui->n_btn = 0;
        head_draw( ui, mid, 0.f, false );
        for( int d = 0; d < DJ_DECKS; d++ ) deck_draw( ui, mid, d );
        mixer_lamps( ui, mid );
        next_draw( ui, mid );
        moods_draw( ui, mid );
        req_draw( ui, mid );
        status_draw( ui, mid );
        flush( mid );
        ui->mid_dirty = false;
        ui->mid_at = now;
    }
    GdiFlush();
    if( slow ) memcpy( cv->px, mid->px, bytes );
    else for( int i = 0; i < (int)COUNT( ui->fast ); i++ ) copy_box( cv, mid, ui->fast[i] );
    cv->gdi = false;
    limit( cv, ui->fast[0] );
    head_draw( ui, cv, dt, true );
    limit( cv, ui->fast[1] );
    for( int d = 0; d < DJ_DECKS; d++ )
    {
        overview_draw( ui, cv, d );
        zoom_draw( ui, cv, d );
    }
    pill_draw( ui, cv );
    limit( cv, ui->fast[2] );
    mixer_draw( ui, cv );
    for( int d = 0; d < DJ_DECKS; d++ )
    {
        limit( cv, ui->fast[3 + d] );
        deck_lamps( ui, cv, d );
    }
    unlimit( cv );
    GdiFlush();
    cv->gdi = false;
    if( slow ) InvalidateRect( ui->hwnd, NULL, FALSE );
    else for( int i = 0; i < (int)COUNT( ui->fast ); i++ )
    {
        box_t b = ui->fast[i];
        RECT r = { b.x, b.y, b.x + b.w, b.y + b.h };
        InvalidateRect( ui->hwnd, &r, FALSE );
    }
    retitle( ui );
}

/*****************************************************************************
 * The window
 *****************************************************************************/
static void place_save( dj_ui_t *ui )
{
    WINDOWPLACEMENT wp = { .length = sizeof wp };
    if( ui->place_path == NULL || ui->hwnd == NULL || !GetWindowPlacement( ui->hwnd, &wp ) ) return;
    bool max = wp.showCmd == SW_SHOWMAXIMIZED || (IsIconic( ui->hwnd ) && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if( IsWindowVisible( ui->hwnd ) ) ui->maximized = max;
    FILE *f = vlc_fopen( ui->place_path, "wb" );
    if( f == NULL ) return;
    fprintf( f, "%ld %ld %ld %ld %d %d\n", (long)wp.rcNormalPosition.left, (long)wp.rcNormalPosition.top,
             (long)wp.rcNormalPosition.right, (long)wp.rcNormalPosition.bottom, ui->maximized ? 1 : 0,
             (int)lroundf( ui->zoom_s * 10.f ) );
    fclose( f );
}

static bool place_load( dj_ui_t *ui, RECT *r )
{
    char *t = slurp( ui->place_path );
    long a, b, c, d;
    int max = 0, zoom = 0;
    bool ok = t && sscanf( t, "%ld %ld %ld %ld %d %d", &a, &b, &c, &d, &max, &zoom ) >= 5
           && c - a >= 400 && d - b >= 300;
    free( t );
    if( !ok ) return false;
    *r = (RECT){ a, b, c, d };
    ui->maximized = max != 0;
    if( zoom >= 30 && zoom <= 480 ) ui->zoom_s = zoom / 10.f;
    return MonitorFromRect( r, MONITOR_DEFAULTTONULL ) != NULL;
}

typedef UINT (WINAPI *dpi_win_fn)( HWND );

static void dpi_check( dj_ui_t *ui )
{
    unsigned dpi = ui->dpi_of ? ui->dpi_of( ui->hwnd ) : 96;
    if( dpi == ui->dpi ) return;
    ui->dpi = dpi;
    ui->s = dpi / 96.f;
    retheme( ui );
}

typedef struct { HWND self, found; } vlc_find_t;

static BOOL CALLBACK FindVlc( HWND h, LPARAM lp )
{
    vlc_find_t *f = (vlc_find_t *)lp;
    DWORD pid = 0;
    RECT r;
    GetWindowThreadProcessId( h, &pid );
    if( pid != GetCurrentProcessId() || h == f->self || !IsWindowVisible( h ) || GetWindow( h, GW_OWNER )
     || !GetWindowRect( h, &r ) || r.right - r.left < 300 || r.bottom - r.top < 100 )
        return TRUE;
    f->found = h;
    return FALSE;
}

/* the first time, nothing saved: centred on the monitor VLC's own window is on, else the pointer's */
static void place_default( dj_ui_t *ui )
{
    vlc_find_t f = { ui->hwnd, NULL };
    EnumWindows( FindVlc, (LPARAM)&f );
    POINT pt = { 0, 0 };
    if( f.found == NULL ) GetCursorPos( &pt );
    HMONITOR mon = f.found ? MonitorFromWindow( f.found, MONITOR_DEFAULTTONEAREST )
                           : MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY );
    MONITORINFO mi = { .cbSize = sizeof mi };
    WINDOWPLACEMENT wp = { .length = sizeof wp };
    if( !GetMonitorInfoW( mon, &mi ) || !GetWindowPlacement( ui->hwnd, &wp ) ) return;
    for( int pass = 0; pass < 2; pass++ )   /* again at the monitor's own scale */
    {
        RECT a = mi.rcWork;
        int w = __MIN( S( ui, 1400 ), a.right - a.left ), h = __MIN( S( ui, 880 ), a.bottom - a.top );
        int x = (a.left + a.right - w) / 2 - mi.rcWork.left + mi.rcMonitor.left;
        int y = (a.top + a.bottom - h) / 2 - mi.rcWork.top + mi.rcMonitor.top;
        wp.rcNormalPosition = (RECT){ x, y, x + w, y + h };
        wp.showCmd = SW_HIDE;
        SetWindowPlacement( ui->hwnd, &wp );
        unsigned was = ui->dpi;
        dpi_check( ui );
        if( ui->dpi == was ) break;
    }
}

static void resize( dj_ui_t *ui, int w, int h )
{
    if( w <= 0 || h <= 0 ) return;
    if( canvas_size( &ui->frame, w, h ) && canvas_size( &ui->mid, w, h ) && canvas_size( &ui->chrome, w, h ) )
        layout( ui );
}

static void show( dj_ui_t *ui )
{
    if( !IsWindowVisible( ui->hwnd ) ) ShowWindow( ui->hwnd, ui->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL );
    else if( IsIconic( ui->hwnd ) ) ShowWindow( ui->hwnd, SW_RESTORE );
    SetForegroundWindow( ui->hwnd );
}

static int hit( const dj_ui_t *ui, int x, int y )
{
    for( int i = ui->n_btn - 1; i >= 0; i-- )
    {
        box_t r = ui->btn[i].r;
        if( x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h ) return i;
    }
    return -1;
}

static void hover_at( dj_ui_t *ui, int x, int y )
{
    int i = hit( ui, x, y ), id = i >= 0 ? ui->btn[i].id : 0, arg = i >= 0 ? ui->btn[i].arg : 0;
    if( id != ui->hover_id || arg != ui->hover_arg ) ui->mid_dirty = true;
    ui->hover_id = id;
    ui->hover_arg = arg;
}

static LRESULT CALLBACK EditProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    dj_ui_t *ui = (dj_ui_t *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    if( msg == WM_KEYDOWN && wp == VK_RETURN )
    {
        request( ui, GetKeyState( VK_SHIFT ) < 0 );
        return 0;
    }
    if( msg == WM_KEYDOWN && wp == VK_ESCAPE )
    {
        SetWindowTextW( hwnd, L"" );
        SetFocus( ui->hwnd );
        return 0;
    }
    if( msg == WM_CHAR && (wp == 13 || wp == 27) ) return 0;   /* no beep */
    if( msg == WM_SETFOCUS || msg == WM_KILLFOCUS )
    {
        ui->edit_focus = msg == WM_SETFOCUS;
        ui->mid_dirty = true;
    }
    return CallWindowProcW( ui->edit_proc, hwnd, msg, wp, lp );
}

static LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    dj_ui_t *ui = (dj_ui_t *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    if( msg == WM_NCCREATE )
    {
        ui = ((CREATESTRUCTW *)lp)->lpCreateParams;
        ui->hwnd = hwnd;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)ui );
    }
    if( ui == NULL ) return DefWindowProcW( hwnd, msg, wp, lp );
    switch( msg )
    {
        case WM_SIZE:
            if( wp != SIZE_MINIMIZED ) resize( ui, LOWORD( lp ), HIWORD( lp ) );
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO *)lp)->ptMinTrackSize.x = S( ui, 1100 );
            ((MINMAXINFO *)lp)->ptMinTrackSize.y = S( ui, 720 );
            return 0;
        case WM_DPICHANGED:
        {
            const RECT *r = (const RECT *)lp;
            ui->dpi = HIWORD( wp );
            ui->s = ui->dpi / 96.f;
            retheme( ui );
            SetWindowPos( hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                          SWP_NOZORDER | SWP_NOACTIVATE );
            return 0;
        }
        case WM_TIMER:
            if( wp == ID_PLACE )
            {
                KillTimer( hwnd, ID_PLACE );
                if( ui->unplaced ) place_default( ui );
                ui->unplaced = false;
                show( ui );
                return 0;
            }
            if( IsWindowVisible( hwnd ) && !IsIconic( hwnd ) ) frame( ui );
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint( hwnd, &ps );
            const RECT *r = &ps.rcPaint;   /* the update region clips it to what moved */
            if( ui->frame.px )
                BitBlt( dc, r->left, r->top, r->right - r->left, r->bottom - r->top, ui->frame.dc, r->left, r->top, SRCCOPY );
            EndPaint( hwnd, &ps );
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            hover_at( ui, GET_X_LPARAM( lp ), GET_Y_LPARAM( lp ) );
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, hwnd, 0 };
            TrackMouseEvent( &t );
            return 0;
        }
        case WM_MOUSELEAVE:
            ui->hover_id = ui->hover_arg = 0;
            ui->mid_dirty = true;
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus( hwnd );
            SetCapture( hwnd );
            hover_at( ui, GET_X_LPARAM( lp ), GET_Y_LPARAM( lp ) );
            ui->press_id = ui->hover_id;
            ui->press_arg = ui->hover_arg;
            ui->mid_dirty = true;
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            hover_at( ui, GET_X_LPARAM( lp ), GET_Y_LPARAM( lp ) );
            if( ui->press_id && ui->press_id == ui->hover_id && ui->press_arg == ui->hover_arg )
                click( ui, ui->press_id, ui->press_arg );
            ui->press_id = ui->press_arg = 0;
            ui->mid_dirty = true;
            return 0;
        case WM_MOUSEWHEEL:
        {
            POINT p = { GET_X_LPARAM( lp ), GET_Y_LPARAM( lp ) };
            ScreenToClient( hwnd, &p );
            box_t w = ui->waves;
            if( p.x >= w.x && p.x < w.x + w.w && p.y >= w.y && p.y < w.y + w.h )   /* zoom the records */
                ui->zoom_s = clampf( ui->zoom_s * (GET_WHEEL_DELTA_WPARAM( wp ) > 0 ? .8f : 1.25f ), 3.f, 48.f );
            return 0;
        }
        case WM_SETCURSOR:
            if( LOWORD( lp ) == HTCLIENT && ui->hover_id )
            {
                SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_HAND ) );
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT:
            SetTextColor( (HDC)wp, cref( ui->c[C_TEXT] ) );
            SetBkColor( (HDC)wp, cref( ui->c[C_BG] ) );
            return (LRESULT)ui->edit_brush;
        case WM_KEYDOWN:
            if( wp == VK_ESCAPE ) { place_save( ui ); ShowWindow( hwnd, SW_HIDE ); return 0; }
            break;
        case WM_CLOSE:   /* hidden, not gone: View > DJ brings it back */
            place_save( ui );
            ShowWindow( hwnd, SW_HIDE );
            return 0;
        case WM_APP_SHOW:   /* the first time, once VLC's own window is up */
            if( ui->unplaced ) SetTimer( hwnd, ID_PLACE, 1200, NULL );
            else show( ui );
            return 0;
        case WM_APP_QUIT:
            if( IsWindowVisible( hwnd ) ) place_save( ui );
            DestroyWindow( hwnd );
            return 0;
        case WM_DESTROY:
            KillTimer( hwnd, ID_FRAME );
            PostQuitMessage( 0 );
            return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

typedef HANDLE (WINAPI *dpi_ctx_fn)( HANDLE );

static void up( dj_ui_t *ui, int v )
{
    vlc_mutex_lock( &ui->lock );
    ui->up = v;
    vlc_cond_signal( &ui->cond );
    vlc_mutex_unlock( &ui->lock );
}

static void *UiThread( void *data )
{
    dj_ui_t *ui = data;
    HMODULE user32 = GetModuleHandleW( L"user32.dll" );
    dpi_ctx_fn set_ctx = (dpi_ctx_fn)(void *)GetProcAddress( user32, "SetThreadDpiAwarenessContext" );
    ui->dpi_of = (dpi_win_fn)(void *)GetProcAddress( user32, "GetDpiForWindow" );
    if( set_ctx ) set_ctx( (HANDLE)(intptr_t)-4 );   /* per monitor, v2: this thread's windows, not VLC's */
    GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCWSTR)(void *)UiThread, &ui->inst );
    theme_load( ui, -1 );
    ui->s = 1.f;

    WNDCLASSEXW wc;
    memset( &wc, 0, sizeof wc );
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ui->inst;
    wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    wc.lpszClassName = CLASS_NAME;
    if( !RegisterClassExW( &wc ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
    {
        up( ui, -1 );
        return NULL;
    }
    HWND hwnd = CreateWindowExW( WS_EX_APPWINDOW, CLASS_NAME, L"VLC DJ", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 1360, 860, NULL, NULL, ui->inst, ui );
    if( hwnd == NULL )
    {
        UnregisterClassW( CLASS_NAME, ui->inst );
        up( ui, -1 );
        return NULL;
    }
    ui->dpi = ui->dpi_of ? ui->dpi_of( hwnd ) : 96;
    ui->s = ui->dpi / 96.f;
    ui->edit = CreateWindowExW( 0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                (HMENU)(intptr_t)ID_EDIT, ui->inst, NULL );
    if( ui->edit )
    {
        SetWindowLongPtrW( ui->edit, GWLP_USERDATA, (LONG_PTR)ui );
        ui->edit_proc = (WNDPROC)SetWindowLongPtrW( ui->edit, GWLP_WNDPROC, (LONG_PTR)EditProc );
        SendMessageW( ui->edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"A mood, an artist, a station, any words" );
    }
    retheme( ui );

    WINDOWPLACEMENT wp = { .length = sizeof wp };
    RECT r;
    if( GetWindowPlacement( hwnd, &wp ) && place_load( ui, &r ) )
    {
        wp.rcNormalPosition = r;
        wp.showCmd = SW_HIDE;
        SetWindowPlacement( hwnd, &wp );
        dpi_check( ui );
    }
    else ui->unplaced = true;
    RECT cr;
    GetClientRect( hwnd, &cr );
    resize( ui, cr.right, cr.bottom );
    SetTimer( hwnd, ID_FRAME, 1000 / ui->fps, NULL );
    up( ui, 1 );

    MSG msg;
    while( GetMessageW( &msg, NULL, 0, 0 ) > 0 )
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    if( ui->have_st ) dj_mix_StatusClean( &ui->st );
    canvas_free( &ui->frame );
    canvas_free( &ui->mid );
    canvas_free( &ui->chrome );
    fonts_free( ui );
    if( ui->edit_brush ) DeleteObject( ui->edit_brush );
    if( ui->icon_big ) DestroyIcon( ui->icon_big );
    if( ui->icon_small ) DestroyIcon( ui->icon_small );
    if( ui->dwm ) FreeLibrary( ui->dwm );
    UnregisterClassW( CLASS_NAME, ui->inst );
    return NULL;
}

/*****************************************************************************
 * The API
 *****************************************************************************/
static void ui_free( dj_ui_t *ui )
{
    vlc_cond_destroy( &ui->cond );
    vlc_mutex_destroy( &ui->lock );
    free( ui->theme_path );
    free( ui->place_path );
    free( ui->wl );
    free( ui->wf );
    free( ui );
}

dj_ui_t *dj_ui_New( vlc_object_t *obj, dj_mix_t *mix, dj_brain_cb brain, dj_command_cb cmd, void *opaque,
                    const char *theme_path )
{
    dj_ui_t *ui = calloc( 1, sizeof *ui );
    if( ui == NULL ) return NULL;
    ui->obj = obj;
    ui->mix = mix;
    ui->brain_cb = brain;
    ui->command_cb = cmd;
    ui->opaque = opaque;
    ui->theme_path = theme_path ? strdup( theme_path ) : NULL;
    char *dir = config_GetUserDir( VLC_CONFIG_DIR );
    if( dir == NULL || asprintf( &ui->place_path, "%s" DIR_SEP "dj-deck.txt", dir ) < 0 ) ui->place_path = NULL;
    free( dir );
    ui->zoom_s = 12.f;
    vlc_mutex_init( &ui->lock );
    vlc_cond_init( &ui->cond );
    if( vlc_clone( &ui->thread, UiThread, ui, VLC_THREAD_PRIORITY_LOW ) )
    {
        ui_free( ui );
        return NULL;
    }
    vlc_mutex_lock( &ui->lock );
    while( ui->up == 0 ) vlc_cond_wait( &ui->cond, &ui->lock );
    int v = ui->up;
    vlc_mutex_unlock( &ui->lock );
    if( v < 0 )
    {
        msg_Warn( obj, "the deck window did not open" );
        vlc_join( ui->thread, NULL );
        ui_free( ui );
        return NULL;
    }
    return ui;
}

void dj_ui_Show( dj_ui_t *ui )
{
    if( ui ) PostMessageW( ui->hwnd, WM_APP_SHOW, 0, 0 );
}

void dj_ui_Delete( dj_ui_t *ui )
{
    if( ui == NULL ) return;
    PostMessageW( ui->hwnd, WM_APP_QUIT, 0, 0 );
    vlc_join( ui->thread, NULL );
    ui_free( ui );
}

#endif /* _WIN32 */
