#include <Server/Structs/PlayerStruct.h>
#include <Util/Enums.h>
#include <Util/Weapon.h>
#include <Server/Proto10/Rockets.h>
#include <classes/classes.h>
#include <Server/Classes.h>
#include <Server/Server.h>

/* one row per gun: what a full life starts with, what one trigger pull scans
   for and how long the gun rests between two of them */
typedef struct weapon_stats {
    int                      weapon;
    weapon_default_clip_t    clip;
    weapon_default_reserve_t reserve;
    uint8_t                  pellets;
    uint64_t                 delay_nano;
} weapon_stats_t;

static const weapon_stats_t weapon_table[] = {
    {WEAPON_RIFLE, RIFLE_DEFAULT_CLIP, RIFLE_DEFAULT_RESERVE, 1, NANO_IN_MILLI * 500},
    {WEAPON_SMG, SMG_DEFAULT_CLIP, SMG_DEFAULT_RESERVE, 1, NANO_IN_MILLI * 100},
    {WEAPON_SHOTGUN, SHOTGUN_DEFAULT_CLIP, SHOTGUN_DEFAULT_RESERVE, 8, NANO_IN_MILLI * 1000},
    {GAME_WEAPON_ROCKET, ROCKET_CLIP, ROCKET_RESERVE, 0, (uint64_t) (ROCKET_INTERVAL * NANO_IN_SECOND)},
    {GAME_WEAPON_REVOLVER, GAME_REVOLVER_CLIP, GAME_REVOLVER_RESERVE, 1, NANO_IN_MILLI * GAME_REVOLVER_DELAY_MS},
};

static const weapon_stats_t sniper_stats = {
    WEAPON_RIFLE, GAME_SNIPER_CLIP, GAME_SNIPER_RESERVE, 1, NANO_IN_MILLI * GAME_SNIPER_DELAY_MS};

/* the rifle of a Marksman, whose class table gives the sniper */
int weapon_class_sniper(player_t* player)
{
    if (player == NULL) {
        return 0;
    }
    const struct game_class_info* c = classes_of(get_server(), player);
    return c != NULL && c->weapon == WEAPON_RIFLE;
}

/* the gun in hand is that sniper (not the revolver beside it) */
int weapon_is_sniper(player_t* player)
{
    return player != NULL && player->weapon == WEAPON_RIFLE && weapon_class_sniper(player);
}

static const weapon_stats_t* _weapon_stats(int weapon)
{
    const size_t count = sizeof(weapon_table) / sizeof(weapon_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (weapon_table[i].weapon == weapon) {
            return &weapon_table[i];
        }
    }
    return NULL;
}

void set_default_player_ammo(player_t* player)
{
    const weapon_stats_t* stats = weapon_is_sniper(player) ? &sniper_stats : _weapon_stats(player->weapon);
    if (stats == NULL) {
        return;
    }

    player->weapon_clip     = (uint8_t) stats->clip;
    player->weapon_reserve  = (uint8_t) stats->reserve;
    player->default_clip    = stats->clip;
    player->default_reserve = stats->reserve;
}

void set_default_player_ammo_reserve(player_t* player)
{
    const weapon_stats_t* stats = weapon_is_sniper(player) ? &sniper_stats : _weapon_stats(player->weapon);
    if (stats == NULL) {
        return;
    }

    player->weapon_reserve = (uint8_t) stats->reserve;
}

/* not inline: the header declares it without that, which makes it an
   external definition, and an inline function of external linkage may not
   call a static helper of this file (clang's -Wstatic-in-inline) */
void set_default_player_pellets(player_t* player)
{
    /* a weapon the table does not know scans nothing */
    const weapon_stats_t* stats   = _weapon_stats(player->weapon);
    const uint8_t         pellets = stats == NULL ? 0 : stats->pellets;

    if (player->weapon_pellets != pellets) {
        player->weapon_pellets = pellets;
    }
}

uint64_t get_player_weapon_delay_nano(player_t* player)
{
    const weapon_stats_t* stats = weapon_is_sniper(player) ? &sniper_stats : _weapon_stats(player->weapon);

    return stats == NULL ? 0 : stats->delay_nano;
}
