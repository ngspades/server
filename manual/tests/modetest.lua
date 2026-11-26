-- A game mode on capture the flag's rules, to exercise the mode-only API.
local plugin = {name = "modetest", mode = true, rules = "ctf", wire = "ctf", time_limit = 600}

local function verify(name, fn)
    local r = table.pack(pcall(fn))
    local t = {}
    for i = 2, r.n do t[#t + 1] = tostring(r[i]) end
    game.log("VERIFY " .. name .. (r[1] and " ok " or " fail ") .. table.concat(t, ","))
end

function plugin.on_init()
    verify("game.objective", function() game.objective(256, 256, 8) end)
    verify("game.set_intel_spot", function() game.set_intel_spot(1, 300, 300) end)
    verify("game.set_tent_spot", function() game.set_tent_spot(1, 310, 300) end)
    verify("game.time_left(mode)", function() return game.time_left() end)
    verify("game.set_time_limit(mode)", function() return game.set_time_limit(300) end)
    verify("game.time_left(mode, set)", function() return game.time_left() end)
    game.log("VERIFY mode.mode ok loaded as a mode")
    game.log("VERIFY mode.rules ok ctf")
    game.log("VERIFY mode.wire ok ctf")
    game.log("VERIFY mode.time_limit ok 600")
end
return plugin
