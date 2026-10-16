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
 * VLC's Qt interface docks the deck in its main window the way it docks a video: the deck stays
 * the DJ's own window, drawn on the DJ's own thread, a child of the host's. While a deck runs,
 * the playlist's address variable DJ_HOST_VAR points at a dj_host_t; NULL once it is gone.
 */

#ifndef VLC_DJ_HOST_H
#define VLC_DJ_HOST_H 1

#define DJ_HOST_VAR "dj-host"

typedef struct dj_host
{
    /* into parent, an HWND, filling it; show( opaque ) comes on the deck's thread when someone
       asks for the deck: the DJ starting, View > DJ, dj.ps1 deck */
    bool (*dock)( void *parent, void (*show)( void *opaque ), void *opaque );
    /* a window of its own again, hidden; returns once it is */
    void (*undock)( void );
} dj_host_t;

#endif
