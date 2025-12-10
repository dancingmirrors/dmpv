local msg = require 'mp.msg'
local utils = require 'mp.utils'

local viewpoint = {
    yaw = 0.0,
    pitch = -20.0,
    fov = 120.0
}

local res = 4.0 -- 1.0 to 6.0

-- Ranges are limited to prevent seeing edges.
local FOV_MIN = 30.0
local FOV_MAX = 130.0
local PITCH_MIN = -30.0
local PITCH_MAX = 30.0
local YAW_MIN = -15.0
local YAW_MAX = 15.0

local projection_mode = "hequirect"
local input_stereo = "sbs"

local function clamp(value, min_val, max_val)
    return math.max(min_val, math.min(max_val, value))
end

local function wrap_angle(angle)
    while angle > 180.0 do
        angle = angle - 360.0
    end
    while angle < -180.0 do
        angle = angle + 360.0
    end
    return angle
end

local function clip_viewpoint()
    viewpoint.yaw = clamp(viewpoint.yaw, YAW_MIN, YAW_MAX)
    viewpoint.pitch = clamp(viewpoint.pitch, PITCH_MIN, PITCH_MAX)
    viewpoint.fov = clamp(viewpoint.fov, FOV_MIN, FOV_MAX)
end

local function deg2rad(deg)
    return deg * math.pi / 180.0
end

local function update_filter()
    clip_viewpoint()

    pcall(function() mp.command("no-osd sync vf remove @vrrev:v360=") end)

    local stereo_param = "in_stereo=sbs"
    local projection_param = "hequirect"

    local width = res * 160.0
    local height = res * 160.0

    local filter_parts = {
        "no-osd sync vf add @vrrev:v360=",
        projection_param,
        ":flat:",
        stereo_param,
        ":out_stereo=2d:id_fov=180.0:d_fov=",
        tostring(viewpoint.fov),
        ":yaw=",
        tostring(viewpoint.yaw),
        ":pitch=",
        tostring(viewpoint.pitch),
        ":w=",
        tostring(width),
        ":h=",
        tostring(height)
    }
    local filter_cmd = table.concat(filter_parts)

    local ok, err = mp.command(filter_cmd)
    if not ok then
        msg.error("Failed to apply filter: " .. (err or "unknown error"))
    end

    local osd_msg = string.format(
        "360° View | Yaw: %.1f° | Pitch: %.1f° | FOV: %.1f°",
        viewpoint.yaw,
        viewpoint.pitch,
        viewpoint.fov
    )
    mp.osd_message(osd_msg, 1)
end

local function adjust_yaw(delta)
    viewpoint.yaw = viewpoint.yaw + delta
    update_filter()
end

local function adjust_pitch(delta)
    viewpoint.pitch = viewpoint.pitch + delta
    update_filter()
end

local function adjust_fov(delta)
    viewpoint.fov = viewpoint.fov + delta
    update_filter()
end

local function adjust_res(delta)
    res = math.max(1.0, math.min(6.0, res + delta))
    update_filter()
end

local function reset_viewpoint()
    viewpoint.yaw = 0.0
    viewpoint.pitch = -20.0
    viewpoint.fov = 120.0
    res = 4.0
    update_filter()
    mp.osd_message("Viewpoint reset", 1)
end

local function on_video_loaded()
    update_filter()
end

mp.add_forced_key_binding("i", "sbs-pitch-up", function() adjust_pitch(5.0) end, {repeatable = false})
mp.add_forced_key_binding("k", "sbs-pitch-down", function() adjust_pitch(-5.0) end, {repeatable = false})

mp.add_key_binding("j", "sbs-yaw-left", function() adjust_yaw(-5.0) end, {repeatable = false})
mp.add_key_binding("l", "sbs-yaw-right", function() adjust_yaw(5.0) end, {repeatable = false})

mp.add_forced_key_binding("+", "sbs-res-up", function() adjust_res(1.0) end, {repeatable = false})
mp.add_forced_key_binding("_", "sbs-res-down", function() adjust_res(-1.0) end, {repeatable = false})
mp.add_forced_key_binding("=", "sbs-zoom-in", function() adjust_fov(-10.0) end, {repeatable = false})
mp.add_forced_key_binding("-", "sbs-zoom-out", function() adjust_fov(10.0) end, {repeatable = false})
mp.add_forced_key_binding("BS", "sbs-reset", reset_viewpoint)

-- Some keybindings don't seem to work properly outside fullscreen.
mp.set_property("fullscreen", "yes")
mp.set_property("hwdec", "auto-copy")
mp.set_property("sws-fast", "yes")

mp.register_event("file-loaded", on_video_loaded)
