/*****************************************************************************
 * dj_host.h : the DJ's deck, docked in another window
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
 * VLC's Qt interface shows the deck as a page of its main window, in its own widgets: it reads
 * what the deck shows and sends the verbs its buttons are. While a host is attached the DJ's own
 * deck window stays hidden. While a deck runs, the playlist's address variable DJ_HOST_VAR
 * points at a dj_host_t; NULL once it is gone. Every call but attach and detach answers false
 * once the DJ is gone.
 */

#ifndef VLC_DJ_HOST_H
#define VLC_DJ_HOST_H 1

#include "dj.h"

#define DJ_HOST_VAR "dj-host"

typedef struct dj_host
{
    /* show( opaque ) comes on the DJ's thread when someone asks for the deck: the DJ starting,
       View > DJ, dj.ps1 deck */
    bool (*attach)( void (*show)( void *opaque ), void *opaque );
    void (*detach)( void );   /* returns once the DJ will not call show again */
    bool (*status)( dj_status_t *, dj_brain_t * );   /* the status freed by status_clean */
    void (*status_clean)( dj_status_t * );
    bool (*wave)( int deck, double from_s, double to_s, int n, float *low, float *full );   /* as dj_mix_Wave */
    bool (*command)( const char *verb );   /* as the HTTP routes take them: "next", "air?on=1" */
} dj_host_t;

#endif
