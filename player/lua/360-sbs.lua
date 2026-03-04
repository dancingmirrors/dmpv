-- Some ideas stolen from VLC's excellent implementation.
-- Requires libplacebo.

local SHADER_SRC = [[
//!PARAM yaw
//!DESC Horizontal view angle (degrees, positive = right)
//!TYPE DYNAMIC float
//!MINIMUM -180.0
//!MAXIMUM 180.0
0.0

//!PARAM pitch
//!DESC Vertical view angle (degrees, positive = up)
//!TYPE DYNAMIC float
//!MINIMUM -90.0
//!MAXIMUM 90.0
0.0

//!PARAM hfov
//!DESC Horizontal field of view (degrees)
//!TYPE DYNAMIC float
//!MINIMUM 10.0
//!MAXIMUM 170.0
90.0

//!HOOK MAIN
//!BIND HOOKED
//!DESC 360 SBS equirectangular projection
//!WIDTH OUTPUT.w
//!HEIGHT OUTPUT.h

#define PI 3.14159265358979323846

vec4 hook() {
    vec2 ndc = HOOKED_pos * 2.0 - 1.0;
    ndc.y    = -ndc.y;

    float aspect = target_size.x / target_size.y;

    float hfov_rad = hfov * (PI / 180.0);
    float vlon = ndc.x * hfov_rad * 0.5;
    float vlat = ndc.y * hfov_rad * 0.5 / aspect;
    vec3 ray = vec3(sin(vlon) * cos(vlat),
                    sin(vlat),
                    cos(vlon) * cos(vlat));

    float p  = pitch * (PI / 180.0);
    float cp = cos(p), sp = sin(p);
    mat3 Rx = mat3(
        1.0, 0.0,  0.0,   // col 0
        0.0,  cp,  -sp,   // col 1
        0.0,  sp,   cp    // col 2
    );

    float ya = yaw * (PI / 180.0);
    float cy = cos(ya), sy = sin(ya);
    mat3 Ry = mat3(
         cy, 0.0, -sy,    // col 0
        0.0, 1.0,  0.0,   // col 1
         sy, 0.0,  cy     // col 2
    );

    vec3 dir = Ry * Rx * ray;

    float lon = atan(dir.x, dir.z);              // [-PI,   PI  ]
    float lat = asin(clamp(dir.y, -1.0, 1.0));   // [-PI/2, PI/2]
    float u   = lon / (2.0 * PI) + 0.5;          // [ 0,    1   ]
    float v   = 0.5 - lat / PI;                  // [ 0,    1   ] top = north pole

    return HOOKED_tex(vec2(u, v));
}
]]

local yaw   = 0.0
local pitch = 0.0
local hfov  = 90.0

local shader_file = nil

local function norm_yaw(y)
    y = y % 360.0
    if y > 180.0 then y = y - 360.0 end
    return y
end

local function update()
    mp.set_property("glsl-shader-opts",
        string.format("yaw=%.6f,pitch=%.6f,hfov=%.6f",
            norm_yaw(yaw), pitch, hfov))
end

local function increment_pitch()
    pitch = math.min(pitch + 5.0, 90.0)
    update()
end
local function decrement_pitch()
    pitch = math.max(pitch - 5.0, -90.0)
    update()
end

local function increment_yaw()
    yaw = yaw + 5.0
    update()
end
local function decrement_yaw()
    yaw = yaw - 5.0
    update()
end

local function increment_zoom()
    hfov = math.max(hfov - 10.0, 10.0)
    update()
end
local function decrement_zoom()
    hfov = math.min(hfov + 10.0, 170.0)
    update()
end

mp.add_forced_key_binding("i", increment_pitch, 'nonrepeatable')
mp.add_forced_key_binding("k", decrement_pitch, 'nonrepeatable')

mp.add_key_binding("l", increment_yaw, 'nonrepeatable')
mp.add_key_binding("j", decrement_yaw, 'nonrepeatable')

mp.add_forced_key_binding("=", increment_zoom, 'nonrepeatable')
mp.add_forced_key_binding("-", decrement_zoom, 'nonrepeatable')

local drag_last_x = nil
local drag_last_y = nil

mp.add_forced_key_binding("MOUSE_BTN0", "drag_pan", function(event)
    if event.event == "down" then
        local pos = mp.get_property_native("mouse-pos")
        if pos then
            drag_last_x = pos.x
            drag_last_y = pos.y
        end
    elseif event.event == "up" then
        drag_last_x = nil
        drag_last_y = nil
    end
end, {complex = true})

mp.observe_property("mouse-pos", "native", function(_, pos)
    if drag_last_x == nil or not pos or not pos.hover then return end
    local dx = pos.x - drag_last_x
    local dy = pos.y - drag_last_y
    drag_last_x = pos.x
    drag_last_y = pos.y
    if dx == 0 and dy == 0 then return end
    local w = mp.get_osd_size()
    if not w or w == 0 then return end
    local deg_per_px = hfov / w * 3.0
    yaw   = norm_yaw(yaw   + dx * deg_per_px)
    pitch = math.max(-90.0, math.min(90.0, pitch - dy * deg_per_px))
    update()
end)

mp.set_property("correct-downscaling", "yes")
mp.set_property("dscale", "ewa_hanning")
mp.set_property("dscale-blur", "1.11")
mp.set_property("scale", "ewa_hanning")
mp.set_property("scale-blur", "1.11")
mp.set_property("fullscreen", "yes")
mp.set_property("interpolation", "no")
mp.set_property("load-positioning", "no")
mp.set_property("load-360-sg", "no")

-- XXX
local function load_shader()
    local tag = string.format("%d-%06d", os.time(), math.random(0, 999999))
    shader_file = string.format("/tmp/dmpv-360-sbs-%s.glsl", tag)
    local f = io.open(shader_file, "w")
    if not f then
        mp.msg.error("360-sbs: Failed to create tmp shader file: " .. shader_file)
        shader_file = nil
        return
    end
    f:write(SHADER_SRC)
    f:close()
    mp.commandv("change-list", "glsl-shaders", "append", shader_file)
    update()
end

mp.register_event("file-loaded", function()
    yaw   = 0.0
    pitch = 0.0
    hfov  = 90.0
    update()
    local stereo = mp.get_property("video-params/stereo-in") or ""
    if stereo == "sbs2l" or stereo == "sbs2r" then
        -- XXX
        mp.msg.info("360-sbs: SBS stereo metadata detected.")
    end
end)

mp.register_event("shutdown", function()
    if shader_file then
        mp.commandv("change-list", "glsl-shaders", "remove", shader_file)
        os.remove(shader_file)
        shader_file = nil
    end
end)

load_shader()
