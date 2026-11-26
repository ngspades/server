-- Exercises the plugin API on a real server and logs one line per name:
--   VERIFY <name> ok|fail <what it returned>
-- tools/run_api_test.py starts the server with this plugin and turns the
-- lines into gen/verified.typ. Not for production.
local plugin = {}

local function show(...)
    local t = {}
    for i = 1, select("#", ...) do
        local v = select(i, ...)
        if type(v) == "table" then v = "{table}" end
        t[#t + 1] = tostring(v)
    end
    return table.concat(t, ",")
end

local function verify(name, fn)
    local r = table.pack(pcall(fn))
    if r[1] then
        game.log("VERIFY " .. name .. " ok " .. show(table.unpack(r, 2, r.n)))
    else
        game.log("VERIFY " .. name .. " fail " .. tostring(r[2]))
    end
end

-- events: the first call of each is logged
local seen = {}
local function ev(name, ...)
    if not seen[name] then
        seen[name] = true
        game.log("VERIFY " .. name .. " ok " .. show(...))
    end
end

local hooks = {"on_join", "on_team", "on_leave", "on_spawn", "on_kill", "on_damage", "on_hit", "on_build",
    "on_destroy", "on_chat", "on_tool", "on_weapon", "on_class", "on_grenade", "on_flashlight", "on_explode",
    "on_reload", "on_intel", "on_player_tick", "on_login", "on_spawn_point", "allows_respawn"}
for _, h in ipairs(hooks) do
    plugin[h] = function(...) ev(h, ...) end
end
function plugin.on_init() ev("on_init") end
function plugin.on_tick() ev("on_tick") end
function plugin.on_message(from, topic, data) ev("on_message", from, topic, data and data.n) end
function plugin.on_bot_think(bot)
    ev("on_bot_think", bot.id)
    if bot.id % 2 == 0 then
        local x, y = game.spawn(bot.team ~= 0 and bot.team or 1)
        return {name = "walk", x = x, y = y, hold = 3, role = "defend", intent = {"post", "home"}}
    end
end
function plugin.on_bot_arrived(bot, name) ev("on_bot_arrived", bot.id, name) end
function plugin.on_bot_stuck(bot, why) ev("on_bot_stuck", bot.id, why) end
function plugin.on_bot_team_tick(team, bots) ev("on_bot_team_tick", team, #bots) end

local done = false
local function run_checks()
    if done then return end
    done = true
    verify("game.players", function() local l = game.players(); return #l end)
    local list = game.players()
    local p = list[1]
    verify("game.player", function() return game.player(p.id).name end)
    verify("game.find", function() return game.find(p.name).id end)
    verify("game.notice", function() game.notice("api test") end)
    verify("game.fog", function() game.fog(10, 20, 30) end)
    verify("game.get_fog", function() return game.get_fog() end)
    verify("game.drop_crate", function() local x, y = game.spawn(1); return game.drop_crate(x, y, "ammo") end)
    verify("game.score", function() return game.score(1) end)
    verify("game.set_score", function() game.set_score(1, 3); return game.score(1) end)
    verify("game.score_limit", function() return game.score_limit() end)
    verify("game.set_score_limit", function() return game.set_score_limit(7) end)
    verify("game.set_capture_limit", function() return game.set_capture_limit(8) end)
    verify("game.set_time_limit", function() return game.set_time_limit(100) end)
    verify("game.time_left", function() return game.time_left() end)
    verify("game.team_name", function() game.team_name(1, "Reds"); return "ok" end)
    verify("game.team_color", function() game.team_color(2, 1, 2, 3); return "ok" end)
    verify("game.sky", function() local s = game.sky(); return s.time, s.rate, s.weather, s.intensity, s.moonlight end)
    verify("game.set_sky", function() return game.set_sky{time = "dusk", weather = "rain", intensity = 40} end)
    verify("game.set_team_outfit", function() return game.set_team_outfit(1, 0) end)
    verify("game.cell", function() return game.cell(10, 10, 62) end)
    verify("game.color", function() local x, y = game.spawn(1); local z = game.ground(math.floor(x), math.floor(y)); return game.color(math.floor(x), math.floor(y), z) end)
    verify("game.time", function() return game.time() end)
    verify("game.unix_time", function() return game.unix_time() end)
    verify("game.later", function() game.later(0.5, function() game.log("VERIFY game.later ok fired") end) end)
    verify("game.log", function() game.log("log line") end)
    verify("game.map_size", function() return game.map_size() end)
    verify("game.ground", function() return game.ground(256, 256) end)
    verify("game.spawn", function() return game.spawn(1) end)
    verify("game.objective(not a mode)", function() game.objective(100, 100, 5) end)
    verify("game.json_encode", function() return game.json_encode({a = 1, b = {1, 2, 3}, c = "x"}) end)
    verify("game.json_decode", function() local v = game.json_decode('{"a":[1,2,{"b":true}]}'); return v.a[3].b end)
    verify("game.json_decode(bad)", function() return game.json_decode("{") end)
    verify("game.url_encode", function() return game.url_encode("a b&c=d/é") end)
    verify("game.send", function() game.send("ping", {n = 5}) end)
    verify("game.server_info", function() local i = game.server_info(); return i.name, i.map, i.mode, i.players, i.max_players, i.port end)
    verify("game.classes", function() local c = game.classes(); return #c, c[1].name end)
    verify("game.class", function() local c = game.class("miner"); return c.name, c.n, c.health, c.weapon, c.dig end)
    verify("game.set_class", function() return game.set_class("miner", {health = 150, speed = 1.1}) end)
    verify("game.set_class(bad)", function() return game.set_class("miner", {health = 99999}) end)
    verify("game.set_mode(denied)", function() return game.set_mode("ctf") end)
    verify("game.set_map(denied)", function() return game.set_map("Busan_Port") end)
    verify("game.next_map(denied)", function() return game.next_map() end)
    verify("game.run_command(denied)", function() return game.run_command("/say hi") end)
    verify("game.store", function() game.store.n = (game.store.n or 0) + 1; return game.store.n end)
    verify("game.store_save", function() game.store_save() end)
    verify("game.command", function()
        return game.command("hello", {help = "Says hello"}, function(pl, args, line)
            game.log("VERIFY game.command ok ran " .. tostring(pl) .. " args=" .. #args .. " line=" .. line)
            return "hello " .. (pl and pl.name or "console")
        end)
    end)
    game.later(1, function()
        verify("game.run_command", function() return game.run_command("/hello a b") end)
    end)
    verify("game.command(taken)", function() return game.command("kick", function() end) end)
    verify("game.command(role)", function() return game.command("wipe", {role = "admin"}, function() end) end)
    verify("game.set_block", function() return game.set_block(5, 5, 20, 255, 0, 0) end)
    verify("game.set_block(water)", function() return game.set_block(5, 5, 63, 255, 0, 0) end)
    verify("game.clear_block", function() return game.clear_block(5, 5, 20) end)
    verify("game.http(denied)", function() return game.http{url = "https://example.org/"} end)
    verify("game.http", function()
        return game.http{url = "http://127.0.0.1:18765/x", method = "POST", json = {a = 1}, timeout = 5,
            on_done = function(status, body, err) game.log("VERIFY http.on_done ok " .. status .. " " .. tostring(body) .. " " .. tostring(err)) end}
    end)
    verify("game.supported_profiles(late)", function() game.supported_profiles("10") end)

    -- the player object
    verify("p.fields", function()
        local names = {}
        for k in pairs(p) do names[#names + 1] = k end
        table.sort(names)
        return table.concat(names, " ")
    end)
    verify("p:message", function() p:message("hi") end)
    verify("p:set_hp", function() p:set_hp(50) end)
    verify("p:damage", function() p:damage(1, p) end)
    verify("p:teleport", function() p:teleport(p.x, p.y, p.z) end)
    verify("p:spawn_heli", function() return p:spawn_heli() end)
    verify("p:restock", function() p:restock() end)
    verify("p:fog", function() p:fog(1, 2, 3) end)
    verify("p:drop_crate", function() return p:drop_crate("health") end)
    verify("p:set_outfit", function() return p:set_outfit(0) end)
    verify("p:outfit", function() return p:outfit() end)
    verify("p:has_role", function() return p:has_role("admin") end)
    verify("p:sees", function() return p:sees(list[2] or p) end)
    verify("p:kick(bot)", function() return p:kick("x") end)
    verify("p:set_class", function() return p:set_class("miner") end)
    verify("p:set_team", function() return p:set_team(p.team) end)
    verify("p:set_tool", function() return p:set_tool(2) end)
    verify("p:set_weapon", function() return p:set_weapon(0) end)
    verify("p:set_ammo", function() p:set_ammo(5, 10) end)
    verify("p:set_blocks", function() p:set_blocks(50) end)
    verify("p:set_grenades", function() p:set_grenades(2) end)
    verify("p:set_score", function() p:set_score(5) end)
    verify("p:set_kills", function() p:set_kills(5) end)
    verify("p:set_deaths", function() p:set_deaths(5) end)
    verify("p:set_assists", function() p:set_assists(5) end)
    verify("p:set_squad", function() return p:set_squad(1) end)
    verify("p:set_jetpack", function() return p:set_jetpack(true) end)
    verify("p:kill", function() p:kill() end)
    verify("game.ban(bot)", function() return game.ban(p, 1, "x") end)
end

-- the checks run a little after the match starts, with the bots in
game.later(3, run_checks)
return plugin
