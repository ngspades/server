-- bottest_base: bottest_idle without on_bot_think, the baseline it is
-- measured against (server/tools/bottest/README.md). Never for players.
local mode = {name = "bottest_base", wire = "ctf", mode = true, rules = "ctf"}

return mode
