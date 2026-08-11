--!strict
-- Driving a Cubism model from script.
--
-- Everything here goes through the live2d_* services the module exposes. The
-- engine knows nothing about them: they arrive because project.json asks for
-- the live2d module, and the declarations host.luau checks against are written
-- into the generated host_api.luau by the build.

local host = require("./host_api")

-- The node src/game.cpp made. Looked up once and kept: find walks the scene.
local model: number? = nil

local elapsed = 0
local next_motion = 0

-- Motions live in named groups, and the names are the export's own: Hiyori
-- has nine under "Idle" and one under "TapBody". An empty group name matches
-- nothing, which costs a warning per frame and a model that never moves.
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

			-- A motion every few seconds, in turn. live2d_play answers false
			-- when the model has not finished loading, which is ordinary for
			-- the first frame or two - so this simply tries again next frame.
			if elapsed >= next_motion then
				local turn = math.floor(elapsed / EVERY)
				-- Every fourth one is the tap reaction, so the group name is
				-- visibly doing something rather than being scenery.
				local group, index = "Idle", turn % IDLE
				if turn % 4 == 3 then
					group, index = "TapBody", 0
				end
				if host.live2d_play(who, group, index, false) then
					next_motion = elapsed + EVERY
				end
			end

			-- Look around: the two parameters every Cubism model has by
			-- convention. A parameter the model does not carry is answered
			-- false rather than being an error, so this is safe on any export.
			host.live2d_set_param(who, "ParamAngleX", math.sin(elapsed * 0.7) * 25)
			host.live2d_set_param(who, "ParamAngleY", math.sin(elapsed * 0.4) * 12)

			-- And speak, roughly. Lip sync normally comes from a voice - see
			-- live2d_speak - but the mouth is drivable on its own.
			host.live2d_mouth(who, math.max(0, math.sin(elapsed * 6)) * 0.8)
		end,
	},
}
