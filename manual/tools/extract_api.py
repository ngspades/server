#!/usr/bin/env python3
"""Scan the server's Lua bindings and write api-surface.txt: the checklist
of every name the plugin sandbox exposes. `make check` fails when the
reference (chapters/*.typ) lacks one of them. Each line: kind<TAB>name."""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parents[2]
src = root / "server/Source/Server"
plugins_c = (src / "Plugins.c").read_text()
luamode_c = (src / "Gamemodes/LuaMode.c").read_text()
classes_h = (root / "common/classes/classes.h").read_text()

out = []

def add(kind, name):
    out.append((kind, name))

def block(text, start_re, end="};"):
    m = re.search(start_re, text)
    if not m:
        sys.exit("pattern not found: " + start_re)
    i = text.index(end, m.end())
    return text[m.end():i]

# game.* functions: the luaL_Reg table
for n in re.findall(r'\{"(\w+)",\s*api_\w+\}', block(plugins_c, r"static const luaL_Reg game_api\[\] = \{")):
    add("function", "game." + n)
add("function", "game.store")  # a field set at load, not a function

# player methods
for n in re.findall(r'\{"(\w+)",\s*m_\w+\}', block(plugins_c, r"static const luaL_Reg player_methods\[\] = \{")):
    add("method", "p:" + n)

# player fields: lua_setfield calls inside _push_player
body = plugins_c[plugins_c.index("static void _push_player"):plugins_c.index("static player_t* _self")]
for n in dict.fromkeys(re.findall(r'lua_setfield\(L, -2, "(\w+)"\)', body)):
    add("field", "p." + n)

# events: every on_* the C code asks, plus the two non-on_ ones
events = set(re.findall(r'"(on_[a-z_]+)"', plugins_c)) | set(re.findall(r'"(on_[a-z_]+)"', luamode_c))
events.add("allows_respawn")
events.discard("on_done")
for n in sorted(events):
    add("event", n)

# a mode's own fields read from the returned table
for n in sorted(set(re.findall(r'plugins_mode_(?:field|flag|number)\(lm\.plugin,\s*"(\w+)"', luamode_c))):
    add("mode-field", "mode." + n)
add("mode-field", "mode.mode")  # `mode = true` declares a game mode

# bot goal keys: _num_at/_bool_at/_point_at/getfield of the goal table
gb = plugins_c[plugins_c.index("static void _goal_from_table"):plugins_c.index("static int _goal_guarded")]
keys = set(re.findall(r'(?:_num_at|_bool_at)\(L, (?:t|lua_gettop\(L\)), "(\w+)"', gb))
keys |= set(re.findall(r'_point_at\(L, t, "(\w+)"', gb))
keys |= set(re.findall(r'lua_getfield\(L, t, "(\w+)"\)', gb))
keys |= {"dig", "color"}  # inside `work`
for n in sorted(keys):
    add("goal-key", "goal." + n)
for n in re.findall(r'\{"(\w+)", GM_INTENT_\w+\}', gb.replace("\n", "") + plugins_c[plugins_c.index("intent_names"):plugins_c.index("static uint16_t _intent_of")].replace("\n", "")):
    add("intent", "intent." + n)
rm = re.search(r'static const char\* roles\[\] = \{([^}]*)\}', gb)
for n in re.findall(r'"(\w+)"', rm.group(1)):
    add("role", "role." + n)

# tables returned to the plugin
sky = plugins_c[plugins_c.index("static int api_sky"):plugins_c.index("static int api_set_sky")]
for n in re.findall(r'lua_setfield\(L, -2, "(\w+)"\)', sky):
    add("sky-field", "sky." + n)
for k in ("time", "rate", "weather", "intensity", "moonlight"):
    add("set-sky-key", "set_sky." + k)
info = plugins_c[plugins_c.index("static int api_server_info"):plugins_c.index("/* --- the network: game.http")]
for n in re.findall(r'lua_setfield\(L, -2, "(\w+)"\)', info):
    add("info-field", "info." + n)
cls = re.search(r'game_class_key\(int k\)[^{]*\{\s*static const char\* keys\[[^\]]*\] = \{(.*?)\};', classes_h, re.S)
for n in re.findall(r'"(\w+)"', cls.group(1)):
    add("class-field", "class." + n)
add("class-field", "class.name")
add("class-field", "class.n")

# game.http request keys and the callback
for n in ("url", "method", "headers", "body", "json", "timeout", "on_done"):
    add("http-key", "http." + n)

# the operator's side
for n in ("load", "allow_map_change", "allow_commands"):
    add("config", "plugins." + n)
for n in ("allow_hosts", "profiles"):
    add("config", "plugin." + n)
for n in sorted(set(re.findall(r'getenv\("(SP_PLUGIN\w*)"\)', plugins_c))):
    add("env", n)

# the EVT lines of the plugin system
for n in sorted(set(re.findall(r'evt_log\([^;]*?"plugin",\s*"action=(\w+)', plugins_c, re.S))):
    add("evt", "action=" + n)

seen = set()
lines = []
for kind, name in out:
    if name not in seen:
        seen.add(name)
        lines.append(f"{kind}\t{name}")
dest = root / "manual/api-surface.txt"
dest.write_text("\n".join(lines) + "\n")
print(f"{len(lines)} names -> {dest}")
