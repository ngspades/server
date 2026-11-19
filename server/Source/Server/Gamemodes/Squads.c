// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Profile.h>
#include <Server/Gamemodes/Ctf.h>
#include <Server/Gamemodes/Squads.h>
#include <Server/Player.h>
#include <Server/Proto10/Session.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <uthash/uthash.h>
#include <enet/enet.h>
#include <proto10/codec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>

/* a mate that was hit or fired this recently is in a fight, and so is one
   with an enemy this close: spawning next to it walks into the fight (the
   first test respawned bots one block from where they had just died) */
#define SQUAD_FIGHT_NANOS (3ULL * NANO_IN_SECOND)
#define SQUAD_ENEMY_RANGE 20.0f

static const char* squad_names[SQUADS_MAX + 1] = {"none",  "Alfa",  "Bravo", "Charlie", "Delta",    "Echo",
                                                   "Foxtrot", "Golf", "Hotel", "India",  "Juliett", "Kilo",
                                                   "Lima",  "Mike",  "November", "Oscar", "Papa"};

static struct
{
    uint8_t loaded;
    uint8_t enabled;
    uint8_t size;
} cfg;

static void _load_config(void)
{
    if (cfg.loaded) {
        return;
    }
    cfg.loaded  = 1;
    cfg.enabled = 1;
    cfg.size    = 4;
    FILE* fp    = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* table = root != NULL ? toml_table_in(root, "squads") : NULL;
        if (table != NULL) {
            toml_datum_t d = toml_bool_in(table, "enabled");
            if (d.ok) {
                cfg.enabled = d.u.b ? 1 : 0;
            }
            d = toml_int_in(table, "size");
            if (d.ok && d.u.i >= 2 && d.u.i <= 16) {
                cfg.size = (uint8_t) d.u.i;
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_SQUADS");
    if (env != NULL && *env) {
        cfg.enabled = atoi(env) != 0;
    }
    LOG_INFO("Squads: %s (up to %u players)", cfg.enabled ? "on" : "off", cfg.size);
}

static int _in_play(const player_t* p)
{
    return (p->team == TEAM_A || p->team == TEAM_B) && p->state != STATE_DISCONNECTED &&
           p->state != STATE_STARTING_MAP;
}

static int _members(server_t* server, uint8_t team, uint8_t squad, const player_t* except)
{
    int       n = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != except && _in_play(p) && p->team == team && p->squad_team == team && p->squad == squad) {
            n++;
        }
    }
    return n;
}

/* the 0.75 profile has no squads on the wire (docs/plan-075-server.md 6.2):
   the squads stay as the server's bookkeeping, because the bots' lanes are
   walked by squad (Ctf.c's leaders, followers and posts), but no client is
   ever told of one, and nobody spawns by a mate or a rally point */
static void _send(player_t* p, const p10_writer* w)
{
    if (w->error || p->conn == NULL || profile_is_075() || p->protocol != PROTOCOL_10 ||
        p->p10_state != P10_STATE_GAME)
    {
        return;
    }
    conn_send(p->conn, 0, w->data, w->size, 1);
}

static void _write_squad(p10_writer* w, uint8_t* buf, size_t size, const player_t* p)
{
    char id[8], squad[8];
    snprintf(id, sizeof id, "%u", p->id);
    snprintf(squad, sizeof squad, "%u", p->squad);
    p10_writer_init(w, buf, size);
    p10_start_generic_command(w);
    p10_put_generic_part(w, "squad");
    p10_put_generic_part(w, id);
    p10_put_generic_part(w, squad);
}

static void _set_squad(server_t* server, player_t* player, uint8_t squad, const char* why)
{
    player->squad      = squad;
    player->squad_team = player->team;
    evt_log(player, "squad", "action=join squad=%s team=%u why=%s", squad_names[squad], player->team, why);
    uint8_t    buf[48];
    p10_writer w;
    _write_squad(&w, buf, sizeof buf, player);
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        _send(p, &w);
    }
}

/* the fullest squad of the team with room, a new one only when every squad
   is full: the smallest with room spread a team over squads of 1 and 2,
   and a squad of one is a player alone */
static uint8_t _pick_squad(server_t* server, player_t* player)
{
    uint8_t best = 0, empty = 0;
    int     best_n = 0;
    for (uint8_t s = 1; s <= SQUADS_MAX; s++) {
        int n = _members(server, player->team, s, player);
        if (n == 0) {
            if (empty == 0) {
                empty = s;
            }
        } else if (n < cfg.size && n > best_n) {
            best   = s;
            best_n = n;
        }
    }
    return best ? best : empty;
}

/* a bot respawning in a squad smaller than another of its team with room
   moves there (a mate left for a role, died out of the team, or was
   removed): two squads of one stayed apart all match. Never a human: the
   player picked its squad, or got one and may change it */
static void _fill(server_t* server, player_t* player)
{
    if (!player->is_bot || player->squad == 0 || player->squad_team != player->team) {
        return;
    }
    int     own  = _members(server, player->team, player->squad, player);
    uint8_t best = 0;
    int     best_n = own;
    for (uint8_t s = 1; s <= SQUADS_MAX; s++) {
        int n = _members(server, player->team, s, player);
        if (s != player->squad && n < cfg.size && n > best_n) {
            best   = s;
            best_n = n;
        }
    }
    if (best != 0) {
        _set_squad(server, player, best, "fill");
    }
}

static int _in_fight(server_t* server, const player_t* mate, uint64_t now)
{
    if (now - mate->hurt_at < SQUAD_FIGHT_NANOS || now - mate->timers.since_last_shot < SQUAD_FIGHT_NANOS) {
        return 1;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->team != mate->team && _in_play(p) && p->state == STATE_READY && p->alive) {
            float dx = p->movement.position.x - mate->movement.position.x;
            float dy = p->movement.position.y - mate->movement.position.y;
            float dz = p->movement.position.z - mate->movement.position.z;
            if (dx * dx + dy * dy + dz * dz < SQUAD_ENEMY_RANGE * SQUAD_ENEMY_RANGE) {
                return 1;
            }
        }
    }
    return 0;
}

void squads_leave_for_role(server_t* server, player_t* player)
{
    if (player->squad == 0 && player->squad_team == SQUAD_TEAM_ROLE) {
        return;
    }
    _set_squad(server, player, 0, "role");
    player->squad_team = SQUAD_TEAM_ROLE;
}

void squads_census(server_t* server)
{
    static uint64_t next;
    uint64_t        now = get_nanos();
    _load_config();
    if (!cfg.enabled || now < next) {
        return;
    }
    next = now + 30ULL * NANO_IN_SECOND;
    for (uint8_t team = TEAM_A; team <= TEAM_B; team++) {
        /* sizes[n]: squads of n players; alone: on the team in no squad
           without a role; role: kept out by a role */
        int       sizes[9] = {0}, alone = 0, role = 0, players = 0;
        player_t *p, *tmp;
        HASH_ITER(hh, server->players, p, tmp)
        {
            if (!_in_play(p) || p->team != team) {
                continue;
            }
            players++;
            if (p->squad_team == SQUAD_TEAM_ROLE) {
                role++;
            } else if (p->squad == 0 || p->squad_team != team) {
                alone++;
            }
        }
        for (uint8_t s = 1; s <= SQUADS_MAX; s++) {
            int n = _members(server, team, s, NULL);
            if (n > 0) {
                sizes[n < 8 ? n : 8]++;
            }
        }
        evt_log(NULL, "squad_census", "team=%u players=%d role=%d alone=%d of1=%d of2=%d of3=%d of4=%d", team,
                players, role, alone, sizes[1], sizes[2], sizes[3], sizes[4]);
    }
}

/* the squad's rally point (BF4's spawn beacon): dropped
   by the squad's leader where it stands, "squad-rally" from its client; the
   squad respawns there first for RALLY_SECONDS */
#define RALLY_SECONDS 60
static struct
{
    vector3f_t at;
    uint64_t   until;
} rally[2][SQUADS_MAX + 1];

/* the squad's leader: its first human by id (a bot leads only a squad of
   bots), else its first bot */
static player_t* _leader(server_t* server, uint8_t team, uint8_t squad)
{
    player_t *best = NULL, *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (!_in_play(p) || p->team != team || p->squad_team != team || p->squad != squad) {
            continue;
        }
        if (best == NULL || (best->is_bot && !p->is_bot) || (best->is_bot == p->is_bot && p->id < best->id)) {
            best = p;
        }
    }
    return best;
}

static void _rally_refuse(player_t* player, const char* why)
{
    uint8_t    buf[48];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "squad-refused");
    p10_put_generic_part(&w, why);
    _send(player, &w);
    evt_log(player, "rally", "action=refused why=%s", why);
}

static void _rally_drop(server_t* server, player_t* player)
{
    if (!cfg.enabled || (player->team != TEAM_A && player->team != TEAM_B) || player->squad == 0 ||
        player->squad_team != player->team)
    {
        _rally_refuse(player, "no_squad");
        return;
    }
    if (!gamemode_has(server, GM_FEAT_SQUAD_SPAWN)) {
        _rally_refuse(player, "mode");
        return;
    }
    if (player->state != STATE_READY || !player->alive) {
        _rally_refuse(player, "dead");
        return;
    }
    if (_leader(server, player->team, player->squad) != player) {
        _rally_refuse(player, "not_leader");
        return;
    }
    /* not on the enemy's spawn: a rally there would spawn on top of them */
    quad3d_t*  enemy = &server->protocol.spawns[player->team == TEAM_A ? TEAM_B : TEAM_A];
    vector3f_t pos   = player->movement.position;
    if (pos.x > enemy->from.x - 10.f && pos.x < enemy->to.x + 10.f && pos.y > enemy->from.y - 10.f &&
        pos.y < enemy->to.y + 10.f)
    {
        _rally_refuse(player, "too_close");
        return;
    }
    vector3f_t at;
    if (player->wade || !player_spawn_near(server, pos.x, pos.y, pos.z, &at)) {
        _rally_refuse(player, "no_ground");
        return;
    }
    rally[player->team][player->squad].at    = pos;
    rally[player->team][player->squad].until = get_nanos() + (uint64_t) RALLY_SECONDS * NANO_IN_SECOND;
    char id[8], c[3][16];
    snprintf(id, sizeof id, "%u", (unsigned) player->id);
    snprintf(c[0], sizeof c[0], "%.1f", pos.x);
    snprintf(c[1], sizeof c[1], "%.1f", pos.y);
    snprintf(c[2], sizeof c[2], "%.1f", pos.z);
    uint8_t    buf[96];
    p10_writer w;
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "squad-rally");
    p10_put_generic_part(&w, id);
    p10_put_generic_part(&w, c[0]);
    p10_put_generic_part(&w, c[1]);
    p10_put_generic_part(&w, c[2]);
    int       sent = 0;
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_in_play(p) && p->team == player->team && p->squad_team == p->team && p->squad == player->squad) {
            _send(p, &w);
            sent++;
        }
    }
    evt_log(player, "rally", "action=dropped squad=%s at=%.1f,%.1f,%.1f sent=%d", squad_names[player->squad], pos.x,
            pos.y, pos.z, sent);
}

int squads_spawn_point(server_t* server, player_t* player)
{
    _load_config();
    if (!cfg.enabled || player->team == TEAM_SPECTATOR) {
        return 0;
    }
    if (ctf_bot_has_post(server, player)) {
        squads_leave_for_role(server, player);
        return 0; /* the team's spawn */
    }
    /* a new player, or a team change: a squad of the team it plays for now.
       A player who chose to be alone (squad 0 on its own team) stays alone */
    if (player->squad_team != player->team) {
        _set_squad(server, player, _pick_squad(server, player), "auto");
    } else {
        _fill(server, player);
    }
    if (player->squad == 0 || profile_is_075() || !gamemode_has(server, GM_FEAT_SQUAD_SPAWN)) {
        return 0; /* classic CTF: the team's spawn */
    }
    /* the squad's rally point while it stands */
    if ((player->team == TEAM_A || player->team == TEAM_B) && rally[player->team][player->squad].until > get_nanos()) {
        vector3f_t r = rally[player->team][player->squad].at, at;
        if (player_spawn_near(server, r.x, r.y, r.z, &at)) {
            player->movement.position = at;
            evt_log(player, "squad_spawn", "squad=%s rally=1 at=%.1f,%.1f,%.1f", squad_names[player->squad], at.x,
                    at.y, at.z);
            return 1;
        }
    }
    /* a living mate of the squad, out of the water and out of a fight */
    player_t* mates[256];
    int       count = 0;
    uint64_t  now   = get_nanos();
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p != player && p->team == player->team && p->squad_team == player->team && p->squad == player->squad &&
            p->state == STATE_READY && p->alive && !p->wade && !_in_fight(server, p, now) && count < 256)
        {
            mates[count++] = p;
        }
    }
    while (count > 0) {
        int        k    = (int) (gen_rand(&server->rand) * count) % count;
        player_t*  mate = mates[k];
        vector3f_t at;
        if (player_spawn_near(
            server, mate->movement.position.x, mate->movement.position.y, mate->movement.position.z, &at))
        {
            player->movement.position = at;
            evt_log(player, "squad_spawn", "squad=%s near=%u at=%.1f,%.1f,%.1f", squad_names[player->squad], mate->id,
                    at.x, at.y, at.z);
            return 1;
        }
        mates[k] = mates[--count]; /* walled in: try another mate */
    }
    evt_log(player, "squad_spawn", "squad=%s near=none why=no_free_mate", squad_names[player->squad]);
    return 0;
}

void squads_client_in_game(server_t* server, player_t* player)
{
    _load_config();
    if (profile_is_075()) {
        return; /* no squad wire there (_send would drop it anyway) */
    }
    uint8_t    buf[48];
    p10_writer w;
    char       size[8];
    snprintf(size, sizeof size, "%u", cfg.enabled ? cfg.size : 0);
    p10_writer_init(&w, buf, sizeof buf);
    p10_start_generic_command(&w);
    p10_put_generic_part(&w, "squads");
    p10_put_generic_part(&w, size);
    _send(player, &w);
    if (!cfg.enabled) {
        return;
    }
    player_t *p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (_in_play(p) && p->squad_team == p->team) {
            _write_squad(&w, buf, sizeof buf, p);
            _send(player, &w);
        }
    }
}

int squads_command(server_t* server, player_t* player, const char* command, const char* arg)
{
    /* 1.0 GenericCommands a 0.75 client cannot send: refused, with the why */
    if (profile_is_075() && (strcmp(command, "squad-rally") == 0 || strcmp(command, "squad") == 0)) {
        evt_log(player, "squad", "action=refused command=%s why=profile", command);
        return 1;
    }
    if (strcmp(command, "squad-rally") == 0) {
        _load_config();
        _rally_drop(server, player);
        return 1;
    }
    if (strcmp(command, "squad") != 0) {
        return 0;
    }
    _load_config();
    int squad = atoi(arg);
    if (!cfg.enabled || player->team == TEAM_SPECTATOR || squad < 0 || squad > SQUADS_MAX) {
        evt_log(player, "squad", "action=refused squad=%d why=%s", squad, cfg.enabled ? "invalid" : "disabled");
        return 1;
    }
    if (squad > 0 && _members(server, player->team, (uint8_t) squad, player) >= cfg.size) {
        uint8_t    buf[48];
        p10_writer w;
        p10_writer_init(&w, buf, sizeof buf);
        p10_start_generic_command(&w);
        p10_put_generic_part(&w, "squad-refused");
        p10_put_generic_part(&w, "full");
        _send(player, &w);
        evt_log(player, "squad", "action=refused squad=%s why=full", squad_names[squad]);
        return 1;
    }
    /* takes effect now; the next respawn goes next to the new squad */
    _set_squad(server, player, (uint8_t) squad, "chosen");
    return 1;
}
