
local host = require("./host_api")

local model: number? = nil

local elapsed = 0
local next_motion = 0

local IDLE = 9
local EVERY = 6

return {
	["game.model"] = {
		run = function(ctx: { dt: number })
			if model == nil then
				model = host.find("model")
				if model == nil then
					return
				end
			end
			local who = model :: number

			elapsed += ctx.dt

			if elapsed >= next_motion then
				local turn = math.floor(elapsed / EVERY)
				local group, index = "Idle", turn % IDLE
				if turn % 4 == 3 then
					group, index = "TapBody", 0
				end
				if host.live2d_play(who, group, index, false) then
					next_motion = elapsed + EVERY
				end
			end

			host.live2d_set_param(who, "ParamAngleX", math.sin(elapsed * 0.7) * 25)
			host.live2d_set_param(who, "ParamAngleY", math.sin(elapsed * 0.4) * 12)

			host.live2d_mouth(who, math.max(0, math.sin(elapsed * 6)) * 0.8)
		end,
	},
}
