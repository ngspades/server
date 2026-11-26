-- the queued, opt-in calls (run with SP_PLUGIN_ALLOW_MAP=1 SP_PLUGIN_COMMANDS=say)
local plugin = {}
local function verify(name, fn)
    local r = table.pack(pcall(fn))
    local t = {}
    for i = 2, r.n do t[#t + 1] = tostring(r[i]) end
    game.log("VERIFY " .. name .. (r[1] and " ok " or " fail ") .. table.concat(t, ","))
end
game.later(2, function()
    verify("game.run_command", function() return game.run_command("/say queued hello") end)
    verify("game.run_command(shutdown)", function() return game.run_command("/shutdown") end)
    verify("game.run_command(chain)", function() return game.run_command("/say a; /say b") end)
    verify("game.set_map", function() return game.set_map("Busan_Port") end)
    verify("game.set_map(bad)", function() return game.set_map("../etc") end)
    verify("game.set_mode", function() return game.set_mode("tdm") end)
    verify("game.set_mode(unknown)", function() return game.set_mode("nonesuch") end)
end)
function plugin.on_init() game.log("pendtest init map=" .. game.server_info().map .. " mode=" .. game.server_info().mode) end
return plugin
