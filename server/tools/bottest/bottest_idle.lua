-- bottest_idle: capture the flag with an on_bot_think that always gives the
-- goal back to the mode (nil). It measures what asking a plugin costs:
-- tools/bench.py run against bottest_base (the same mode without the
-- function) plays the same match and differs only in the tick's time
-- (server/tools/bottest/README.md). Never for players.
local mode = {name = "bottest_idle", wire = "ctf", mode = true, rules = "ctf"}

function mode.on_bot_think(bot)
    return nil
end

return mode
