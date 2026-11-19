# bottest: the bots' Lua API under test

Game modes that exist only to test the plugins' bot API (`docs/bots.md` "Asking bots for something"). They are not plugins for
servers: players never see them. The server loads them from here with
`SP_PLUGINS_DIR` (absolute, or relative to the server's root).

| mode | what it is |
| --- | --- |
| `bottest` | a VIP escort. Team 1's living bot with the lowest id is the VIP (`on_bot_team_tick`) and runs for team 2's spawn; the rest of team 1 escort it; team 2 hunts it, else holds its spawn. A VIP that gets there (`on_bot_arrived`) scores and is killed |
| `bottest_idle` | capture the flag whose `on_bot_think` always returns nil |
| `bottest_base` | the same without `on_bot_think`: the baseline |

```sh
export SP_PLUGINS_DIR=$PWD/server/tools/bottest
# the escort plays
python3 tools/bench.py run --bin current --match bottest:2Fort_CTF --seeds 3 --bots 16 \
    --metric 'vip_arrived=vip_arrived' --metric 'errors=plugin action=error'
# what asking a plugin costs: idle against base (the same match), and the escort
python3 tools/bench.py run --bin current --match bottest_base:Busan_Port \
    --match bottest_idle:Busan_Port --match bottest:Busan_Port --seeds 3 --bots 120 --seconds 300 --jobs 3
```

The server logs the API's own time every 30 s: `Plugins: on_bot_think n calls, x us each, y ms in all` (real time, a measurement).

Measured (Apple M4, 2026-10-30):

- **Nil plays the same match.** `bottest_idle` against `bottest_base` gives the same events once the mode's name is set aside: Busan_Port with 120 bots, 85130 events; 2Fort with 16, 12085.
- **Cost of asking.** Busan_Port, 120 bots, 300 s:

  | mode | µs per call | ms a tick | tick |
  | --- | --- | --- | --- |
  | `bottest_base` | | | 0.67 ms |
  | `bottest_idle` | 2.8 | 0.06 | 0.75 ms |
  | `bottest` | 7.3 (it reads the VIP with `game.player` too) | 0.14 | 1.27-1.50 ms |

  The escort's other ~0.6 ms is its game: escorts and hunters follow a moving target and plan 32% more (in `SP_FAST` the planner searches on the tick).
- **The escort plays.**

  | map | VIP runs that get there | kills | plugin errors |
  | --- | --- | --- | --- |
  | 2Fort, 16 bots, 600 s, 3 seeds | 5.3 a match | 285 | 0 |
  | Raindance (bases 330 apart), 16 bots, 600 s, 3 seeds | none (the hunters get the VIP first) | 135 | 0 |
- **Without any plugin, the game is unchanged.** `bench.py check --bin before --bin current` gives identical events on all eight matches of.
