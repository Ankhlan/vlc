--[[
 SoundCloud track pages.

 SoundCloud serves AAC in fragmented-MP4 HLS now; the progressive MP3 this script used to dig
 out of the page is gone, and with it every track it could play. A track page is handed to the
 soundcloud:// access (the dj plugin, modules/control/dj.c), which asks SoundCloud's API for
 the stream when the track plays: a signed stream URL expires, so it is fetched as late as
 possible. Sets and a user's tabs are not one stream and are left alone.

 Copyright © 2012, 2015 the VideoLAN team
 Copyright © 2026 Ankhlan Erdenebat

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
--]]

local tabs = { tracks = true, albums = true, sets = true, reposts = true, likes = true,
               followers = true, following = true, ["popular-tracks"] = true, comments = true }

local function page_path()
    return vlc.path:gsub("^www%.", ""):gsub("^m%.", "")
end

-- Probe function.
function probe()
    if vlc.access ~= "http" and vlc.access ~= "https" then
        return false
    end
    local user, second = string.match( page_path(), "^soundcloud%.com/([^/?#]+)/([^/?#]+)" )
    return user ~= nil and not tabs[second]
end

-- Parse function.
function parse()
    local title, art
    while true do
        local line = vlc.readline()
        if not line then break end
        title = title or string.match( line, '<meta property="og:title" content="(.-)"' )
        art = art or string.match( line, '<meta property="og:image" content="(.-)"' )
    end
    local page = "https://" .. page_path():gsub( "[?#].*$", "" )
    return { { path = "soundcloud://resolve?url=" .. vlc.strings.encode_uri_component( page ),
               name = title and vlc.strings.resolve_xml_special_chars( title ),
               arturl = art } }
end
