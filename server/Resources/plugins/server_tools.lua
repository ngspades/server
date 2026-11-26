-- server_tools.lua: the admin travel and spawn commands, shipped with the
-- server and loaded by config.toml (1.0 profile only: the helicopter and the
-- supply crates are protocol 1.0 entities).
--
--   /spawn              a helicopter on dry ground in front of you
--   /spawn crate        a random ammo or health crate falling from the sky
--                       in front of you
--   /goto <x> <y>       the middle of that 16 block chunk (0 based, the
--                       client's own chunk grid: 32 x 32 on a 512 map,
--                       64 x 64 on a 1024 map)
--
-- Only the admin role (or a higher one: manager) may run them; the host of a
-- listen server enters as admin (Player.c). The map's z grows downwards, and
-- the player's hip stands 2.4 blocks over the floor (Player.c).
game.supported_profiles("10")

-- a crate's kind is drawn per command; a map's fresh Lua state would
-- otherwise repeat the same sequence (Lua 5.4 starts with a fixed seed)
math.randomseed(game.unix_time())

local CHUNK = 16

-- the middle and top of the nearest dry column to x, y within radius;
-- nil when every column there is water, the sky's edge or off the map
local function ground_near(x, y, radius)
    local sx, sy, sz = game.map_size()
    for r = 0, radius do
        local steps = r == 0 and 1 or math.max(8, math.floor(6.2831853 * r))
        for k = 0, steps - 1 do
            local a = 6.2831853 * k / steps
            local px = math.floor(x + math.cos(a) * r)
            local py = math.floor(y + math.sin(a) * r)
            if px >= 0 and py >= 0 and px < sx and py < sy then
                local top = game.ground(px, py)
                if top > 1 and top < sz - 2 then
                    return px + 0.5, py + 0.5, top
                end
            end
        end
    end
    return nil
end

-- a point in front of the player, level with it
local function front(p, distance)
    local ox, oy = p.ox, p.oy
    local len = math.sqrt(ox * ox + oy * oy)
    if len < 0.01 then
        return p.x, p.y
    end
    return p.x + ox / len * distance, p.y + oy / len * distance
end

local function spawn_heli(p)
    local id, why = p:spawn_heli()
    if not id then
        if why == "profile" then
            return "The helicopter needs protocol 1.0"
        end
        if why == "too_many_objects" then
            return "Too many objects on this server"
        end
        return "No room for a helicopter here"
    end
    return "Helicopter incoming"
end

local function spawn_crate(p)
    local fx, fy = front(p, 8)
    local x, y = ground_near(fx, fy, 10)
    if not x then
        return "No room for a crate here"
    end
    local kind = math.random(2) == 1 and "ammo" or "health"
    local ok, why = game.drop_crate(x, y, kind)
    if not ok then
        return "No room for a crate here (" .. tostring(why) .. ")"
    end
    return kind == "ammo" and "Ammo crate incoming" or "Health crate incoming"
end

local function chunk_pair(args)
    if args[1] and args[2] then
        return tonumber(args[1]), tonumber(args[2])
    end
    local x, y = string.match(args[1] or "", "^(%-?%d+)[,;/:](%-?%d+)$")
    return tonumber(x), tonumber(y)
end

local function travel(p, args)
    local cx, cy = chunk_pair(args)
    local sx, sy = game.map_size()
    local nx = math.ceil(sx / CHUNK)
    local ny = math.ceil(sy / CHUNK)
    if not cx or not cy or cx ~= math.floor(cx) or cy ~= math.floor(cy) then
        return "Usage: /goto <chunk x> <chunk y>, 0 to " .. (nx - 1)
    end
    if cx < 0 or cy < 0 or cx >= nx or cy >= ny then
        return "Chunks are 0 to " .. (nx - 1) .. " by 0 to " .. (ny - 1)
    end
    local x, y, top = ground_near(cx * CHUNK + CHUNK / 2, cy * CHUNK + CHUNK / 2, CHUNK / 2 - 1)
    if not x then
        return "That chunk is water"
    end
    p:teleport(x, y, top - 2.4)
    return "Teleported to chunk " .. cx .. "," .. cy
end

game.command("spawn", {
    role = "admin",
    help = "a helicopter in front of you, or /spawn crate for a supply crate",
}, function(p, args)
    if not p then
        return "Only a player can use this"
    end
    local what = string.lower(args[1] or "")
    if what == "" or what == "heli" or what == "helicopter" then
        return spawn_heli(p)
    end
    if what == "crate" then
        return spawn_crate(p)
    end
    return "Usage: /spawn [crate]"
end)

game.command("goto", {
    role = "admin",
    help = "teleport to the middle of a 16 block chunk: /goto <x> <y>",
}, function(p, args)
    if not p then
        return "Only a player can use this"
    end
    return travel(p, args)
end)

-- the loader wants a plugin table; these commands are all it does
return {}
