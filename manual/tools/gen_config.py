#!/usr/bin/env python3
"""Generate gen/config-tables.typ and gen/env.typ from the server's own
sources: the registry of config keys (ConfigKeys.c: every key the server
reads, with its profile), the shipped config.toml / config-075.toml (default
and comment) and the SP_* environment variables the C code reads. A key the
registry knows but the shipped files do not show is listed with the meaning
from tools/meanings.py, so none is missing."""
import re, sys, pathlib, json

root = pathlib.Path(__file__).resolve().parents[2]
here = pathlib.Path(__file__).resolve().parent
ck = (root / "server/Source/Server/ConfigKeys.c").read_text()
sys.path.insert(0, str(here))
from meanings import MEANINGS, SECTION_INTRO, ENV_OVERRIDES, TYPES, DEFAULTS  # noqa

# ---- the registry -------------------------------------------------------
arrays = {}
for m in re.finditer(r"static const ck_key_t (\w+)\[\] = \{(.*?)CK_END", ck, re.S):
    arrays[m.group(1)] = re.findall(r'\{"(\w+)",\s*(CK_\w+)\}', m.group(2))
tops = []
m = re.search(r"static const ck_table_t ck_top_subs\[\] = \{(.*?)\{\.name = NULL\}", ck, re.S)
for e in re.finditer(r'\{\.name = "(\w+)"(.*?)\},?\n', m.group(1)):
    prof = re.search(r"\.prof = (CK_\w+)", e.group(2))
    keys = re.search(r"\.keys = (\w+)", e.group(2))
    tops.append((e.group(1), prof.group(1) if prof else "CK_ALL", keys.group(1) if keys else None))

PROF = {"CK_ALL": "both", "CK_10": "1.0", "CK_075": "0.75"}

# sub-tables with their own key lists
subs = {
    "perks.<perk>": ("perks", arrays["ck_perk_keys"]),
    "teams.team1 / team2": ("teams", [("name", "CK_ALL"), ("color", "CK_ALL")]),
    "profile.075": ("profile", arrays["ck_profile075_keys"]),
    "presets.<name>": ("presets", arrays["ck_preset_keys"]),
}
classes_h = (root / "common/classes/classes.h").read_text()
cm = re.search(r'game_class_key\(int k\)[^{]*\{\s*static const char\* keys\[[^\]]*\] = \{(.*?)\};', classes_h, re.S)
class_keys = re.findall(r'"(\w+)"', cm.group(1))

# ---- the shipped configs -------------------------------------------------
def parse_toml(path):
    """{section: {key: (value text, comment text)}}, plus the comment block above each section"""
    res, sec_comment = {}, {}
    sec, block = "", []
    lines = path.read_text().splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        s = ln.strip()
        if not s:
            block = []
        elif s.startswith("#"):
            block.append(s.lstrip("#").strip())
        elif s.startswith("["):
            sec = s.strip("[]").split("]")[0].strip()
            res.setdefault(sec, {})
            sec_comment[sec] = " ".join(block)
            block = []
        else:
            mm = re.match(r"(\w+)\s*=\s*(.*)", s)
            if mm:
                key, val = mm.group(1), mm.group(2)
                # a multi-line array
                depth = val.count("[") - val.count("]")
                while depth > 0:
                    i += 1
                    val += " " + lines[i].strip()
                    depth = val.count("[") - val.count("]")
                trailing = ""
                tm = re.match(r'^(.*?)\s+#\s*(.*)$', val)
                if tm and tm.group(1).count('"') % 2 == 0:
                    val, trailing = tm.group(1), tm.group(2)
                comment = " ".join(block + ([trailing] if trailing else []))
                res.setdefault(sec, {})[key] = (val.strip(), comment)
                block = []
        i += 1
    # commented-out keys: "# port = 32888" documents a default too
    sec = ""
    for ln in lines:
        s = ln.strip()
        if s.startswith("["):
            sec = s.strip("[]").split("]")[0].strip()
        mm = re.match(r"#\s*(\w+)\s*=\s*(.+)$", s)
        if mm and mm.group(1) not in res.get(sec, {}):
            res.setdefault(sec, {})[mm.group(1)] = ("(unset) " + mm.group(2).strip(), "")
    return res, sec_comment

c10, sc10 = parse_toml(root / "server/Resources/config.toml")
c75, sc75 = parse_toml(root / "server/Resources/config-075.toml")

# ---- the environment variables --------------------------------------------
envs = set()
for p in (root / "server/Source").rglob("*.c"):
    envs |= set(re.findall(r'"(SP_[A-Z0-9_]+)"', p.read_text(errors="ignore")))

def env_for(sec, key):
    if (sec, key) in ENV_OVERRIDES:
        return ENV_OVERRIDES[(sec, key)]
    for cand in (f"SP_{sec}_{key}", f"SP_{key}", f"SP_{sec}_{key}S"):
        if cand.upper() in envs:
            return cand.upper()
    return ""

def esc(s):
    s = s.replace("\\", "\\\\")
    for ch in "#$@*_<>[]~":
        s = s.replace(ch, "\\" + ch)
    return s

def typ(s):
    """text for a Typst content block: `code` spans become raw text"""
    parts = s.split("`")
    out = []
    for i, part in enumerate(parts):
        if i % 2 == 1 and i < len(parts) - 1 or (i % 2 == 1 and len(parts) % 2 == 1):
            out.append('#raw("' + part.replace("\\", "\\\\").replace('"', '\\"') + '")')
        else:
            out.append(esc(part))
    return "".join(out)

def lit(s):
    return "`" + s.replace("`", "'") + "`" if s else '"-"'

def guess_type(val):
    v = val.replace("(unset) ", "")
    if v in ("true", "false"):
        return "bool"
    if v.startswith("["):
        return "array"
    if v.startswith('"'):
        return "string"
    if v.startswith("{"):
        return "table"
    if re.match(r"^-?\d+$", v):
        return "int"
    if re.match(r"^-?[\d.]+$", v):
        return "number"
    return "-"

gaps = []
def rows_for(section, keys, prefix=None):
    rows = []
    name = prefix or section
    for key, prof in keys:
        d10 = c10.get(name, {}).get(key)
        d75 = c75.get(name, {}).get(key)
        # the 0.75-only keys live in config-075.toml
        pick = d75 if (prof == "CK_075" or (d10 is None and d75 is not None)) else d10
        val, com = pick if pick else ("", "")
        override = MEANINGS.get(f"{name}.{key}")
        meaning = override or com
        if not meaning and d75 and d75[1]:
            meaning = d75[1]
        if not meaning:
            gaps.append(f"{name}.{key}")
            meaning = "See the section text."
        default = val
        if d10 and d75 and d10[0] != d75[0] and prof == "CK_ALL":
            default = f"{d10[0]} (1.0); {d75[0]} (0.75)"
        default = DEFAULTS.get(f"{name}.{key}", default)
        if len(default) > 70:
            default = "a list: see the shipped file"
        default = default.replace("(unset) ", "") + (" (commented out in the shipped file)" if val.startswith("(unset)") else "")
        t = TYPES.get(f"{name}.{key}") or guess_type(val)
        rows.append((key, t, default, meaning, PROF[prof], env_for(name.split(".")[0] if "." in name and name.startswith("perks") is False else name.replace(".", "_"), key)))
    return rows

def table(rows):
    o = ["#config-table(("]
    for key, t, default, meaning, prof, env in rows:
        o.append(f"  ({lit(key)}, {lit(t)}, {lit(default)}, [{typ(meaning)}], [{prof}], {lit(env)}),")
    o.append("))")
    return "\n".join(o)

out = ["// generated by tools/gen_config.py from ConfigKeys.c and the shipped configs: do not edit", '#import "/template.typ": *', ""]
for name, prof, arr in tops:
    out.append(f"=== `[{name}]`\n")
    intro = SECTION_INTRO.get(name)
    if not intro:
        intro = (c10.get(name) and sc10.get(name)) or sc75.get(name) or ""
    if intro:
        out.append(typ(intro) + "\n")
    if prof != "CK_ALL":
        out.append(f"Read by the {PROF[prof]} profile only.\n")
    if arr:
        out.append(table(rows_for(name, arrays[arr])) + "\n")
    elif name == "classes":
        out.append("One table per class: `[classes.commando]`, `[classes.rocketeer]`, `[classes.marksman]`, `[classes.miner]`. Every key is optional; a key outside its range is refused with a warning.\n")
        rows = []
        for k in class_keys:
            rows.append((k, TYPES.get("classes." + k, "-"), "per class", MEANINGS.get("classes." + k, ""), "1.0", ""))
            if not rows[-1][3]:
                gaps.append("classes." + k)
                rows[-1] = rows[-1][:3] + ("See the Lua API chapter, Classes.",) + rows[-1][4:]
        out.append(table(rows) + "\n")
    elif name == "presets":
        out.append("A preset is a table `[presets.<name>]`, picked with `--preset <name>`, `[server] preset` or `SP_PRESET`. The shipped files define `ngctf`, `zombie`, `tdm`, `arena`, `babel` and `koth` (1.0) and `ctf` and `babel` (0.75).\n")
        out.append(table(rows_for("presets", arrays["ck_preset_keys"], "presets.ngctf")) + "\n")
    elif name == "teams":
        out.append(table(rows_for("teams", [("name", "CK_ALL"), ("color", "CK_ALL")], "teams")) + "\n")
        out.append("The two teams are written inline: `team1 = { name = \"Blue Team\", color = [62, 153, 234] }`, and `team2` likewise.\n")
    elif name == "plugins":
        out.append(table(rows_for("plugins", arrays["ck_plugins_keys"])) + "\n")
    elif name == "profile":
        out.append("`[profile.075]` holds the tolerant rules a classic player expects; the 0.75 profile applies them over `[conduct]`, `[anticheat]` and `[gameplay]`.\n")
        out.append(table(rows_for("profile", arrays["ck_profile075_keys"], "profile.075")) + "\n")
    if name == "perks":
        out.append("Per perk (`[perks.jetpack]`, `[perks.sentry]`, `[perks.mg]`, `[perks.landmine]`, `[perks.dynamite]`):\n")
        out.append(table(rows_for("perks", arrays["ck_perk_keys"], "perks.jetpack")) + "\n")

pathlib.Path(root / "manual/gen").mkdir(exist_ok=True)
(root / "manual/gen/config-tables.typ").write_text("\n".join(out))

# env table
envrows = []
for e in sorted(envs):
    envrows.append((e, MEANINGS.get("env." + e, "")))
o = ["// generated by tools/gen_config.py: do not edit", '#import "/template.typ": *', "#env-table(("]
for e, m in envrows:
    o.append(f"  ({lit(e)}, [{typ(m) if m else 'Read by the server (see the source).'}]),")
o.append("))")
(root / "manual/gen/env.typ").write_text("\n".join(o))
miss_env = [e for e, m in envrows if not m]
print("keys with no meaning:", gaps)
print("env vars without text:", len(miss_env), "of", len(envrows))
json.dump({"keys": sum(len(v) for v in arrays.values())}, open("/dev/null", "w"))
