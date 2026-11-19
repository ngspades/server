// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Profile.h>
#include <Server/Proto10/NetStats.h>
#include <Server/Classes.h>
#include <Server/EventLog.h>
#include <Server/Perks.h>
#include <Server/Proto10/Jetpack.h>
#include <Server/Proto10/Objects.h>
#include <Server/Proto10/Mg.h>
#include <Server/Proto10/Sentry.h>
#include <Server/Proto10/Explosives.h>
#include <Server/Proto10/Session.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <enet/enet.h>
#include <math.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

/* one perk's earning rules; -1 on a weight means "inherit [perks]" */
typedef struct
{
    int points;         /* progress needed to earn it; 0 = granted at every spawn */
    int kill;           /* progress per enemy kill */
    int pickup;         /* progress per intel pickup */
    int capture;        /* progress per intel capture */
    int enabled;        /* 0: it can never be earned or used */
    int reset_on_death; /* 0: progress survives death (then it is once per round) */
} perk_rule_t;

typedef struct
{
    int         loaded;
    int         kill, pickup, capture; /* [perks] defaults every perk inherits */
    perk_rule_t base[P10_PERK_COUNT];  /* config.toml's */
    perk_rule_t rule[P10_PERK_COUNT];  /* the running mode's: base, or its perk_rules over it */
    const gamemode_ops_t* mode;        /* whose rules are in rule[] */
    int         class_perks;           /* the classes own the perks (the mode's table does not apply) */
} perk_config_t;

static perk_config_t perk_cfg;

static const char* perk_names[P10_PERK_COUNT] = {"jetpack", "sentry", "mg", "landmine", "dynamite"};
/* the thresholds when config.toml says nothing */
static const int perk_default_points[P10_PERK_COUNT] = {3, 5, 4, 3, 4};

static int _perk_read_int(toml_table_t* t, const char* key, int fallback)
{
    if (t == NULL) {
        return fallback;
    }
    toml_datum_t d = toml_int_in(t, key);
    return d.ok && d.u.i >= 0 && d.u.i <= 255 ? (int) d.u.i : fallback;
}

static int _perk_read_bool(toml_table_t* t, const char* key, int fallback)
{
    if (t == NULL) {
        return fallback;
    }
    toml_datum_t d = toml_bool_in(t, key);
    return d.ok ? d.u.b : fallback;
}

static int _perk_env(const char* name, int fallback)
{
    const char* v = getenv(name);
    if (v == NULL || *v == 0) {
        return fallback;
    }
    int n = atoi(v);
    return n >= 0 && n <= 255 ? n : fallback;
}

/* one optional [perks.<name>] table overrides what the perk inherited */
static void _perk_read_table(toml_table_t* sub, perk_rule_t* r)
{
    r->points         = _perk_read_int(sub, "points", r->points);
    r->kill           = _perk_read_int(sub, "kill_points", r->kill);
    r->pickup         = _perk_read_int(sub, "intel_pickup_points", r->pickup);
    r->capture        = _perk_read_int(sub, "intel_capture_points", r->capture);
    r->enabled        = _perk_read_bool(sub, "enabled", r->enabled);
    r->reset_on_death = _perk_read_bool(sub, "reset_on_death", r->reset_on_death);
    /* perks are never activated by the player any more: the jetpack is worn
       when earned, the sentry goes to the tool bar. Say so instead of
       silently ignoring an older config */
    toml_datum_t activation = toml_string_in(sub, "activation");
    if (activation.ok) {
        LOG_WARNING("[perks] activation is no longer used (perks are given automatically), ignoring \"%s\"",
                    activation.u.s);
        free(activation.u.s);
    }
}

/* config.toml [perks] plus one optional [perks.<name>] table per perk;
   SP_PERK_JETPACK and SP_PERK_SENTRY override the thresholds for tests */
static void _perk_load_config(void)
{
    if (perk_cfg.loaded) {
        return;
    }
    perk_cfg.loaded  = 1;
    perk_cfg.kill    = 1;
    perk_cfg.pickup  = 1;
    perk_cfg.capture = 3;
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        perk_cfg.rule[k] = (perk_rule_t) {
            .points         = perk_default_points[k],
            .kill           = -1,
            .pickup         = -1,
            .capture        = -1,
            .enabled        = 1,
            .reset_on_death = 1,
        };
    }

    toml_table_t* root  = NULL;
    toml_table_t* perks = NULL;
    FILE*         fp    = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char err[200];
        root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        if (root != NULL) {
            perks = toml_table_in(root, "perks");
        }
    }
    perk_cfg.kill    = _perk_read_int(perks, "kill_points", perk_cfg.kill);
    perk_cfg.pickup  = _perk_read_int(perks, "intel_pickup_points", perk_cfg.pickup);
    perk_cfg.capture = _perk_read_int(perks, "intel_capture_points", perk_cfg.capture);
    /* the old flat keys still set a threshold when [perks.<name>] is absent */
    perk_cfg.rule[P10_PERK_JETPACK].points =
    _perk_read_int(perks, "jetpack_points", perk_cfg.rule[P10_PERK_JETPACK].points);
    perk_cfg.rule[P10_PERK_SENTRY].points =
    _perk_read_int(perks, "sentry_points", perk_cfg.rule[P10_PERK_SENTRY].points);
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        /* no [perks] table at all (the 0.75 profile's config): the defaults */
        toml_table_t* sub = perks != NULL ? toml_table_in(perks, perk_names[k]) : NULL;
        if (sub != NULL) {
            _perk_read_table(sub, &perk_cfg.rule[k]);
        }
    }
    if (root != NULL) {
        toml_free(root);
    }
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        perk_rule_t* r = &perk_cfg.rule[k];
        if (r->kill < 0) {
            r->kill = perk_cfg.kill;
        }
        if (r->pickup < 0) {
            r->pickup = perk_cfg.pickup;
        }
        if (r->capture < 0) {
            r->capture = perk_cfg.capture;
        }
    }
    perk_cfg.rule[P10_PERK_JETPACK].points = _perk_env("SP_PERK_JETPACK", perk_cfg.rule[P10_PERK_JETPACK].points);
    perk_cfg.rule[P10_PERK_SENTRY].points  = _perk_env("SP_PERK_SENTRY", perk_cfg.rule[P10_PERK_SENTRY].points);
    perk_cfg.rule[P10_PERK_MG].points      = _perk_env("SP_PERK_MG", perk_cfg.rule[P10_PERK_MG].points);
    perk_cfg.rule[P10_PERK_LANDMINE].points =
    _perk_env("SP_PERK_LANDMINE", perk_cfg.rule[P10_PERK_LANDMINE].points);
    perk_cfg.rule[P10_PERK_DYNAMITE].points =
    _perk_env("SP_PERK_DYNAMITE", perk_cfg.rule[P10_PERK_DYNAMITE].points);
    memcpy(perk_cfg.base, perk_cfg.rule, sizeof perk_cfg.base);
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        const perk_rule_t* r = &perk_cfg.rule[k];
        if (!r->enabled) {
            LOG_INFO("Perks: %s disabled", perk_names[k]);
        } else {
            LOG_INFO("Perks: %s at %d (kill %d, pickup %d, capture %d, reset on death %d)",
                     perk_names[k],
                     r->points,
                     r->kill,
                     r->pickup,
                     r->capture,
                     r->reset_on_death);
        }
    }
}

/* the running mode's rules: config.toml's, or the mode's own over them
   (zombie: the jetpack only, for two kills). When the classes own the perks
   (ngCTF, the zombie mode with them) the mode's own table does not apply:
   every class earns its own streak perk, so the mode's jetpack would be a
   second progression on top of it. */
static void _perk_rules(server_t* server)
{
    _perk_load_config();
    const gamemode_ops_t* ops = gamemode_current(server);
    int                class_perks = gamemode_has(server, GM_FEAT_CLASS_PERKS);
    if (perk_cfg.mode == ops && perk_cfg.class_perks == class_perks) {
        return;
    }
    perk_cfg.mode = ops;
    perk_cfg.class_perks = class_perks;
    memcpy(perk_cfg.rule, perk_cfg.base, sizeof perk_cfg.rule);
    if (class_perks || ops->perk_rules == NULL) {
        return;
    }
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        perk_cfg.rule[k].enabled = ops->perk_rules[k].enabled;
        perk_cfg.rule[k].points  = ops->perk_rules[k].points;
        perk_cfg.rule[k].kill    = ops->perk_rules[k].kill;
        evt_log(NULL, "perk", "action=mode_rule mode=%s perk=%s enabled=%u points=%u kill=%u", ops->name,
                perk_names[k], ops->perk_rules[k].enabled, ops->perk_rules[k].points, ops->perk_rules[k].kill);
    }
}

/* what an action is worth for one perk */
typedef enum
{
    PERK_ACTION_KILL,
    PERK_ACTION_PICKUP,
    PERK_ACTION_CAPTURE,
} perk_action_t;

static int _perk_weight(const perk_rule_t* r, perk_action_t action)
{
    switch (action) {
        case PERK_ACTION_KILL: return r->kill;
        case PERK_ACTION_PICKUP: return r->pickup;
        case PERK_ACTION_CAPTURE: return r->capture;
    }
    return 0;
}

/* the mode says whether the jetpack and the sentry exist (only CTF: in
   Babel the jetpack would fly up the tower, an Arena round is about the gun) */
static int _perks_in_mode(server_t* server)
{
    /* the 0.75 profile has no perks (docs/plan-075-server.md 6.2): nothing
       is earned, granted, worn or announced, whatever the mode or the
       config. gamemode_has_perks says no there too; this is the gate every
       grant path of this file goes through (perks_enabled, perks_holds) */
    if (profile_is_075()) {
        return 0;
    }
    return gamemode_has_perks(server);
}

/* in a classes mode a player earns its class's perk only (the Rocketeer's
   jetpack is the class's, not a perk); elsewhere the jetpack and the sentry:
   the MG, the landmine and the dynamite are the classes' own */
static int _perk_for(server_t* server, player_t* p, int perk)
{
    int c = classes_perk(server, p);
    if (c < 0) {
        return perk == P10_PERK_JETPACK || perk == P10_PERK_SENTRY;
    }
    return perk == c;
}

int perks_enabled(server_t* server, int perk)
{
    _perk_rules(server);
    return _perks_in_mode(server) && perk_cfg.rule[perk].enabled;
}

int perks_holds(server_t* server, player_t* player, int perk)
{
    _perk_rules(server);
    return _perks_in_mode(server) && perk_cfg.rule[perk].enabled && _perk_for(server, player, perk) &&
           (player->perk_inventory & (1u << perk));
}

void perks_consume(player_t* player, int perk)
{
    player->perk_inventory &= (uint8_t) ~(1u << perk);
    evt_log(player, "perk", "action=used perk=%s", perk_names[perk]);
}

void perks_class_spawn(server_t* server, player_t* player)
{
    _perk_rules(server);
    int perk = classes_perk(server, player);
    if (perk < 0 || perk >= P10_PERK_COUNT || !_perks_in_mode(server) || !perk_cfg.rule[perk].enabled) {
        return;
    }
    /* in the inventory, not perk_granted: the streak can still earn one
       after this one is put down */
    player->perk_inventory |= (uint8_t) (1u << perk);
    evt_log(player, "perk", "action=spawn_charge perk=%s", perk_names[perk]);
}

/* the admin's /perk: a perk now, whatever the streak says. It is marked
   granted too, so the same life's streak does not earn a second one */
int perks_grant(server_t* server, player_t* player, int perk)
{
    _perk_rules(server);
    if (player == NULL || perk < 0 || perk >= P10_PERK_COUNT || !_perks_in_mode(server) ||
        !perk_cfg.rule[perk].enabled) {
        return 0;
    }
    player->perk_granted |= (uint8_t) (1u << perk);
    /* exactly this one: an old class's charge in the inventory made the
       client's slot send that one, and the server refused it why=class */
    player->perk_inventory = (uint8_t) (1u << perk);
    evt_log(player, "perk", "action=granted perk=%s by=admin", perk_names[perk]);
    return 1;
}

/* the cheapest perk not granted yet this life, -1 when none is left */
static int _perk_next_index(server_t* server, player_t* p)
{
    int best = -1;
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        if (!perk_cfg.rule[k].enabled || (p->perk_granted & (1u << k)) || !_perk_for(server, p, k)) {
            continue;
        }
        if (best < 0 || perk_cfg.rule[k].points < perk_cfg.rule[best].points) {
            best = k;
        }
    }
    return best;
}

static void _perk_grant(server_t* server, player_t* p)
{
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        const perk_rule_t* r = &perk_cfg.rule[k];
        if (!r->enabled || (p->perk_granted & (1u << k)) || !_perk_for(server, p, k)) {
            continue;
        }
        if (p->perk_points[k] >= r->points) {
            p->perk_granted |= (uint8_t) (1u << k);
            p->perk_inventory |= (uint8_t) (1u << k);
            evt_log(p, "perk", "action=earned perk=%s points=%u/%d", perk_names[k], p->perk_points[k], r->points);
        }
    }
}

/* every event feeds every perk its own weight, so each perk can be earned by
   one action only, or by a mix ("3 kills or 1 capture") */
static void _perk_add(server_t* server, player_t* p, perk_action_t action, const char* why)
{
    _perk_load_config();
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        const perk_rule_t* r = &perk_cfg.rule[k];
        if (!r->enabled || !_perk_for(server, p, k)) {
            continue;
        }
        int amount = _perk_weight(r, action);
        if (amount <= 0) {
            continue;
        }
        int points        = p->perk_points[k] + amount;
        p->perk_points[k] = (uint8_t) (points > 255 ? 255 : points);
    }
    evt_log(p, "perk", "action=progress why=%s", why);
    _perk_grant(server, p);
}

/* on_death: a perk with reset_on_death = false keeps its progress and its
   unused charge across lives (so it becomes a once-per-round unlock). A
   sentry earned and not put down yet is the same for everyone: the charge
   waits in the tool bar until its owner is back and places it, so it keeps
   the "earned" mark too (it is not earned a second time while he holds it) */
static void _perk_reset(player_t* p, const char* why, int on_death)
{
    for (int k = 0; k < P10_PERK_COUNT; k++) {
        if (on_death && !perk_cfg.rule[k].reset_on_death) {
            continue;
        }
        int keep_charge = on_death && (k == P10_PERK_SENTRY || k == P10_PERK_MG) &&
                          (p->perk_inventory & (1u << k)) != 0;
        if (p->perk_points[k] || (p->perk_inventory & (1u << k)) || (p->perk_granted & (1u << k))) {
            evt_log(p, "perk", "action=%s why=%s perk=%s points=%u", keep_charge ? "kept" : "reset", why,
                    perk_names[k], p->perk_points[k]);
        }
        p->perk_points[k] = 0;
        if (!keep_charge) {
            p->perk_inventory &= (uint8_t) ~(1u << k);
            p->perk_granted &= (uint8_t) ~(1u << k);
        }
    }
}

/* the class's charge is the class's: changing class takes the old one and
   its progress away (the client's slot otherwise sent a sentry the new
   class does not own, refused why=class), and takes the jetpack off when
   the new class does not wear one */
void perks_class_changed(server_t* server, player_t* player)
{
    _perk_rules(server);
    if (player->perk_inventory || player->perk_granted) {
        _perk_reset(player, "class", 0);
    }
    if (!classes_jetpack(server, player)) {
        jetpack_remove(server, player, "class");
    }
}

void perks_on_kill(server_t* server, player_t* killer, player_t* victim, uint8_t reason)
{
    _perk_rules(server);
    _perk_reset(victim, "death", 1);
    if (!_perks_in_mode(server)) {
        return;
    }
    /* weapon, headshot, spade, grenade. A sentry kill (7) earns nothing, or
       one perk would feed the next one */
    if (killer == NULL || killer == victim || killer->team == victim->team || reason > 3 ||
        (gamemode_player_rules(server, killer) & GM_RULE_NO_PERKS))
    {
        return;
    }
    _perk_add(server, killer, PERK_ACTION_KILL, "kill");
}

void perks_on_intel(server_t* server, player_t* carrier, int capture)
{
    _perk_rules(server);
    if (carrier == NULL || !_perks_in_mode(server)) {
        return;
    }
    _perk_add(server, carrier, capture ? PERK_ACTION_CAPTURE : PERK_ACTION_PICKUP, capture ? "intel_capture" : "intel_pickup");
}

static void _perk_send(player_t* p, const p10_writer* w)
{
    /* a 1.0 packet: never on the 0.75 profile's wire */
    if (w->error || p->conn == NULL || profile_is_075()) {
        return;
    }
    conn_send_reliable(p->conn, w->data, w->size);
}

/* the HUD shows the reason; chat gets it too through the notice */
static void _perk_refuse(player_t* p, uint8_t perk, const char* why)
{
    const char* name = perk < P10_PERK_COUNT ? perk_names[perk] : "unknown";
    evt_log(p, "perk", "action=use_refused perk=%s why=%s", name, why);
    uint8_t    buf[128];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "perk-refused");
    p10_put_generic_part(&w, name);
    p10_put_generic_part(&w, why);
    _perk_send(p, &w);
}

static void _perk_announce(player_t* p, uint8_t perk)
{
    uint8_t    buf[64];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "perk-activated");
    p10_put_generic_part(&w, perk_names[perk]);
    _perk_send(p, &w);
}

/* the jetpack needs no button: it is worn the moment it is earned, or the
   moment its owner is alive again. The sentry stays in the inventory (a tool
   bar slot) until its owner places it with fire, through perks_on_use */
static void _perk_wear_jetpack(server_t* server, player_t* p)
{
    if (!(p->perk_inventory & (1u << P10_PERK_JETPACK)) || p->state != STATE_READY || !p->alive ||
        p->team == TEAM_SPECTATOR)
    {
        return;
    }
    jetpack_grant(server, p);
    p->perk_inventory &= (uint8_t) ~(1u << P10_PERK_JETPACK);
    evt_log(p, "perk", "action=used perk=jetpack auto=1");
    _perk_announce(p, P10_PERK_JETPACK);
}

void perks_on_use(server_t* server, player_t* player, uint64_t param)
{
    uint8_t  perk;
    uint32_t x, y, z;
    if (!p10_perk_param_decode(param, &perk, &x, &y, &z) || perk >= P10_PERK_COUNT) {
        _perk_refuse(player, 255, "unknown_perk");
        return;
    }
    _perk_rules(server);
    if (profile_is_075()) {
        evt_log(player, "perk", "action=use_refused perk=%s why=profile", perk_names[perk]);
        return;
    }
    if (!perk_cfg.rule[perk].enabled || !_perks_in_mode(server)) {
        _perk_refuse(player, perk, "disabled");
        return;
    }
    if (player->state != STATE_READY || !player->alive) {
        _perk_refuse(player, perk, "not_alive");
        return;
    }
    /* the sentry, the MG, the landmine and the dynamite are placed by the
       player; the jetpack is worn by itself */
    if (perk != P10_PERK_SENTRY && perk != P10_PERK_MG && perk != P10_PERK_LANDMINE && perk != P10_PERK_DYNAMITE) {
        _perk_refuse(player, perk, "automatic");
        return;
    }
    if (gamemode_player_rules(server, player) & GM_RULE_NO_PERKS) {
        _perk_refuse(player, perk, "rules");
        return;
    }
    if (!_perk_for(server, player, perk)) {
        _perk_refuse(player, perk, "class");
        return;
    }
    if (!(player->perk_inventory & (1u << perk))) {
        _perk_refuse(player, perk, "not_earned");
        return;
    }
    const char*   why;
    p10_object_t* o = perk == P10_PERK_SENTRY
                      ? sentry_place(server, player, (int) x, (int) y, (int) z, &why)
                      : (perk == P10_PERK_MG ? mg_place(server, player, (int) x, (int) y, (int) z, &why)
                                             : explosives_place(server, player, perk, (int) x, (int) y, (int) z, &why));
    if (o == NULL) {
        /* the perk stays in the inventory: the player tries elsewhere */
        _perk_refuse(player, perk, why);
        return;
    }
    player->perk_inventory &= (uint8_t) ~(1u << perk);
    evt_log(player, "perk", "action=used perk=%s", perk_names[perk]);
}

/* a mode without perks (Babel, Arena) holds none: whatever a CTF match left
   behind (a sentry standing, a jetpack worn, a charge in the inventory, a
   test grant) is taken away, so they are CTF only whatever the path */
static void _perks_strip(server_t* server)
{
    for (int k = 0; k < P10_MAX_OBJECTS; k++) {
        p10_object_t* o = proto10_objects_at(k);
        /* what a perk of the classes put down, never the map's own objects
           (the helicopters have an owner id on the wire and no owner) */
        if (o != NULL && (o->type == P10_ENT_SENTRY || o->type == P10_ENT_MG || o->type == P10_ENT_LANDMINE ||
                          o->type == P10_ENT_DYNAMITE))
        {
            proto10_objects_destroy(server, o, 0, "mode");
        }
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->perk_inventory || p->perk_granted) {
            _perk_reset(p, "mode", 0);
        }
        jetpack_remove(server, p, "mode");
    }
}

void perks_tick(server_t* server)
{
    /* the 0.75 profile: nothing was ever earned, so nothing to strip or send */
    if (profile_is_075()) {
        return;
    }
    _perk_rules(server);
    if (!_perks_in_mode(server)) {
        _perks_strip(server);
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        /* a player the mode keeps from perks (a human turned zombie) loses
           what it earned as a human */
        int no_perks = _perks_in_mode(server) && (gamemode_player_rules(server, p) & GM_RULE_NO_PERKS);
        if (no_perks) {
            if (p->perk_inventory || p->perk_granted || p->perk_points[P10_PERK_JETPACK]) {
                _perk_reset(p, "rules", 0);
            }
            jetpack_remove(server, p, "rules");
        }
        /* a threshold of 0 (config or SP_PERK_*) grants the perk at every
           spawn: free perks, and the quick way to test them. Not to a zombie:
           the grant and the reset above took turns every tick, and a zombie
           put a sentry down from the charge held in between */
        if (p->state == STATE_READY && p->alive && _perks_in_mode(server) && !no_perks) {
            _perk_grant(server, p);
            /* before the state below reaches the client, so the jetpack is
               never shown as held */
            _perk_wear_jetpack(server, p);
        }
        if (p->protocol != PROTOCOL_10 || p->conn == NULL || p->p10_state != P10_STATE_GAME) {
            p->perk_sent = 0;
            continue;
        }
        /* the wire has one progress byte: send the progress of the perk the
           HUD is waiting for, the cheapest one not granted yet */
        int     idx    = _perks_in_mode(server) ? _perk_next_index(server, p) : -1;
        uint8_t points = idx >= 0 ? p->perk_points[idx] : 0;
        uint8_t next   = idx >= 0 ? (uint8_t) perk_cfg.rule[idx].points : 0;
        /* a jetpack waiting for its owner to be alive is not the player's
           to use: the client only ever holds the sentry */
        uint8_t held = _perks_in_mode(server) ? p->perk_inventory & (uint8_t) ~(1u << P10_PERK_JETPACK) : 0;
        if (p->perk_sent && p->perk_sent_points == points && p->perk_sent_inventory == held &&
            p->perk_sent_next == next)
        {
            continue;
        }
        /* the wire's progress byte does not say which perk it counts towards:
           name it first, so the HUD bar can be labelled (old clients ignore
           the unknown command) */
        if (idx >= 0) {
            uint8_t    cbuf[32];
            p10_writer cw;
            p10_writer_init(&cw, cbuf, sizeof cbuf);
            p10_start_generic_command(&cw);
            p10_put_generic_part(&cw, "perk-next");
            p10_put_generic_part(&cw, perk_names[idx]);
            _perk_send(p, &cw);
        }
        /* only the owner needs its perks: one PlayerUpdate item, to it */
        p10_player_item it;
        memset(&it, 0, sizeof it);
        it.player_id      = p->id;
        it.mask           = P10_PU_PERKS;
        it.perk_points    = points;
        it.perk_inventory = held;
        it.perk_next      = next;
        uint8_t    buf[32];
        p10_writer w;
        p10_writer_init(&w, buf, sizeof buf);
        p10_start_player_update(&w);
        p10_put_player_item(&w, &it);
        _perk_send(p, &w);
        p->perk_sent           = 1;
        p->perk_sent_points    = points;
        p->perk_sent_inventory = held;
        p->perk_sent_next      = next;
    }
}
