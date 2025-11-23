-- Based on VR-Reversal by dfaker

local yaw   = 0.0
local pitch = 0.0
local res   = 4.0
local dfov  = 120.0

local update = function ()
    local ok, err = mp.command(string.format("no-osd sync vf add @vrrev:v360=sg:flat:in_stereo=2d:out_stereo=2d:id_fov=240.0:d_fov=%s:yaw=%s:pitch=%s:w=%s*135.0:h=%s*135.0",dfov,yaw,pitch,res,res))
end

local increment_res = function ()
    update()
    res = math.min(res + 1.0, 6.0)
    update()
end
local decrement_res = function ()
    update()
    res = math.max(res - 1.0, 1.0)
    update()
end

local increment_pitch = function ()
    update()
    pitch = math.min(pitch + 5.0, 30.0)
    update()
end
local decrement_pitch = function ()
    update()
    pitch = math.max(pitch - 5.0, -60.0)
    update()
end

local increment_yaw = function ()
    update()
    yaw = math.min(yaw + 5.0, 20.0)
    update()
end
local decrement_yaw = function ()
    update()
    yaw = math.max(yaw - 5.0, -20.0)
    update()
end

local increment_zoom = function ()
    update()
    dfov = math.max(dfov - 10.0, 30.0)
    update()
end
local decrement_zoom = function ()
    update()
    dfov = math.min(dfov + 10.0, 140.0)
    update()
end

mp.add_forced_key_binding("i", increment_pitch, 'nonrepeatable')
mp.add_forced_key_binding("k", decrement_pitch, 'nonrepeatable')

mp.add_key_binding("l", increment_yaw, 'nonrepeatable')
mp.add_key_binding("j", decrement_yaw, 'nonrepeatable')

mp.add_forced_key_binding("+", increment_res, 'nonrepeatable')
mp.add_forced_key_binding("_", decrement_res, 'nonrepeatable')

mp.add_forced_key_binding("=", increment_zoom, 'nonrepeatable')
mp.add_forced_key_binding("-", decrement_zoom, 'nonrepeatable')

mp.set_property("fullscreen", "yes")
mp.set_property("hwdec", "auto-copy")
mp.set_property("sws-fast", "yes")

update()
