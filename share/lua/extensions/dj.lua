--[[
 dj.lua -- View > DJ: the DJ's booth.

 The DJ is the dj interface module (modules/control/dj.c; extraintf=dj, or http:dj for its
 HTTP routes too). View > DJ brings up its deck window (dj_ui.c) and steps aside. Where the
 DJ has no window, this dialog shows what it is doing and passes it requests through the
 playlist variable "dj-command"; the answer comes back in "dj-reply". Slow verbs are queued
 on the DJ's own thread and answer {"queued":true} at once, so this dialog never blocks VLC.

 Nothing at file scope may call out: VLC lists extensions by running this file in a bare
 Lua state (no os, no require, of vlc only msg), and an error there drops it from the menu.

 Copyright (C) 2026 Ankhlan Erdenebat

 This program is free software; you can redistribute it and/or modify it under the terms of
 the GNU General Public License as published by the Free Software Foundation; either version
 2 of the License, or (at your option) any later version.
--]]

local json, d
local w = {}
local moods = {}

function descriptor()
    return {
        title = "DJ",
        version = "1.0",
        author = "Ankhlan Erdenebat",
        shortdesc = "DJ",
        description = "The DJ's booth: on air or off, what plays and why, moods and requests.",
        capabilities = { "input-listener", "meta-listener" },
    }
end

local function call(cmd)
    local pl = vlc.object.playlist()
    if not pl then return nil, "no playlist" end
    pcall(vlc.var.set, pl, "dj-command", cmd)
    local ok, reply = pcall(vlc.var.get, pl, "dj-reply")
    if not ok or not reply then
        return nil, "The DJ is not running: put extraintf=http:dj in vlcrc and restart VLC."
    end
    local r = json.decode(reply)
    if type(r) ~= "table" then return nil, "The DJ answered something unreadable." end
    if r.ok == false then return r, r.error end
    return r
end

local function esc(s)
    return vlc.strings.encode_uri_component(s or "")
end

local function show(r, err)
    if not d then return end
    if not r or not r.state then
        w.head:set_text("<b>DJ</b> - " .. (err or "no answer"))
        d:update()
        return
    end
    local head
    if r.on_air then
        head = "<b>ON AIR</b>"
        if r.block then
            head = head .. " - " .. r.block
            if r.next_block_at then
                head = head .. " until " .. r.next_block_at .. ", then " .. (r.next_block or "")
            end
        end
    else
        head = "<b>off air</b> - the DJ only answers requests"
    end
    w.head:set_text(head)

    local now = "nothing playing"
    if r.state ~= "stopped" then
        now = (r.state == "playing" and "" or (r.state .. ": ")) .. (r.title or "?")
        if r.now_playing and r.now_playing ~= r.title then now = now .. "  |  " .. r.now_playing end
    end
    w.now:set_text(now)

    local who = ""
    if r.owner == "dj" then
        who = "the DJ's pick" .. (r.pick and (": " .. r.pick) or "")
    elseif r.owner == "request" then
        who = "your request" .. (r.held_until and (", held until " .. r.held_until) or "")
    elseif r.owner == "other" then
        who = "not the DJ's music: it waits until this ends"
    end
    w.who:set_text(who)
    w.said:set_text(r.said and ("<i>" .. r.said .. "</i>") or "")
    w.air:set_text(r.on_air and "Off air" or "On air")
    w.vol:set_text("volume " .. tostring(r.volume or "?") .. "%, voice " .. tostring(r.voice or "?"))
    if r.busy then
        w.msg:set_text("working...")
    elseif r.error then
        w.msg:set_text("<font color='#b00'>" .. r.error .. "</font>")
    else
        w.msg:set_text("")
    end
    d:update()
end

local function refresh()
    show(call("state"))
end

local function act(cmd)
    if not d then return end
    local r, err = call(cmd)
    if err then
        w.msg:set_text("<font color='#b00'>" .. err .. "</font>")
        d:update()
        return
    end
    refresh()
end

local function air()
    local r = call("state")
    act("air?on=" .. ((r and r.on_air) and "0" or "1"))
end
local function skip() act("next") end
local function quieter() act("volume?level=-10") end
local function louder() act("volume?level=%2B10") end
local function choose() act("choose") end
local function play_mood()
    local id = w.mood:get_value()
    local m = moods[id]
    if m then act("request?text=" .. esc(m.label)) end
end
local function request(queue)
    local text = w.req:get_text()
    if text and text:match("%S") then
        act("request?text=" .. esc(text) .. (queue and "&queue=1" or ""))
    end
end
local function request_now() request(false) end
local function request_queue() request(true) end

function activate()
    json = require("dkjson")
    local r = call("deck")
    if r and r.ok then
        vlc.deactivate()
        return
    end
    d = vlc.dialog("DJ")
    w.head = d:add_label("", 1, 1, 4, 1)
    w.now = d:add_label("", 1, 2, 4, 1)
    w.who = d:add_label("", 1, 3, 4, 1)
    w.said = d:add_label("", 1, 4, 4, 1)
    w.air = d:add_button("On air", air, 1, 5, 1, 1)
    d:add_button("Skip", skip, 2, 5, 1, 1)
    d:add_button("Quieter", quieter, 3, 5, 1, 1)
    d:add_button("Louder", louder, 4, 5, 1, 1)
    w.mood = d:add_dropdown(1, 6, 3, 1)
    d:add_button("Play mood", play_mood, 4, 6, 1, 1)
    w.req = d:add_text_input("", 1, 7, 2, 1)
    d:add_button("Request", request_now, 3, 7, 1, 1)
    d:add_button("Queue", request_queue, 4, 7, 1, 1)
    d:add_button("Let the DJ choose", choose, 1, 8, 2, 1)
    d:add_button("Refresh", refresh, 3, 8, 1, 1)
    w.vol = d:add_label("", 4, 8, 1, 1)
    w.msg = d:add_label("", 1, 9, 4, 1)
    local p = call("programme")
    if p and p.moods then
        for i, m in ipairs(p.moods) do
            moods[i] = m
            w.mood:add_value(m.label, i)
        end
    end
    d:show()
    refresh()
end

function deactivate()
    d = nil
end

function close()
    vlc.deactivate()
end

function input_changed()
    refresh()
end

function meta_changed()
    refresh()
end
