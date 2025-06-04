-- VR-Reversal by dfaker

local yaw   = 0.0
local pitch = -10.0
local roll  = 0.0
local doit = 0.0
local res  = 4.0
local dragging = false
local dfov=110.0

local draw_cropper = function ()
	local ok, err = mp.command(string.format("async no-osd vf add @vrrev:v360=hequirect:flat:in_stereo=sbs:out_stereo=2d:id_fov=180.0:d_fov=%s:yaw=%s:pitch=%s:roll=%s:w=%s*192.0:h=%s*108.0",dfov,yaw,pitch,roll,res,res))
end

local increment_res = function ()
	res = res+1
	draw_cropper()
end
local decrement_res = function ()
	res = res-1
	res = math.max(1,res)
	draw_cropper()
end

local increment_roll = function ()
	roll = roll+1
	draw_cropper()
end
local decrement_roll = function ()
	roll = roll-1
	draw_cropper()
end

local increment_pitch = function ()
	pitch = pitch+1
	draw_cropper()
end
local decrement_pitch = function ()
	pitch = pitch-1
	draw_cropper()
end

local increment_yaw = function ()
	yaw = yaw+1
	draw_cropper()
end
local decrement_yaw = function ()
	yaw = yaw-1
	draw_cropper()
end

local increment_zoom = function ()
	dfov = dfov-1
	draw_cropper()
end
local decrement_zoom = function ()
	dfov = dfov+1
	draw_cropper()
end

mp.add_forced_key_binding("u", decrement_roll, 'repeatable')
mp.add_forced_key_binding("o", increment_roll, 'repeatable')

mp.add_forced_key_binding("i", increment_pitch, 'repeatable')
mp.add_forced_key_binding("k", decrement_pitch, 'repeatable')

mp.add_key_binding("l", increment_yaw, 'repeatable')
mp.add_key_binding("j", decrement_yaw, 'repeatable')

mp.add_key_binding("c", "easy_crop", draw_cropper)

mp.add_forced_key_binding("+", increment_res, 'repeatable')
mp.add_forced_key_binding("_", decrement_res, 'repeatable')

mp.add_forced_key_binding("=", increment_zoom, 'repeatable')
mp.add_forced_key_binding("-", decrement_zoom, 'repeatable')

mp.set_property("fullscreen", "yes")
mp.set_property("hwdec", "no")
mp.set_property("profile", "low-latency")

draw_cropper()
