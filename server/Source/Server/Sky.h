// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SKY_H
#define SKY_H

#include <Server/Structs/ServerStruct.h>
#include <stdint.h>
#include <Server/Structs/PlayerStruct.h>
#include <tomlc99/toml.h>

/* The time of day and the weather of protocol 1.0 (issue: the ideas of the
   aosprotocol specification's Daytime and Weather, extensions/daytime-weather.md,
   github.com/zerospades/aosprotocol, brought to our protocol). The 0.75
   extension has a day and a night; its Reserved field was kept for a day
   that passes and its Weather bytes for weather. Ours has both: minutes
   since midnight that advance at a rate, and a weather with an intensity.
   Every 1.0 client is told `sky <minutes> <rate> <weather> <intensity>
   <moonlight>` when it enters the game, on a change and every 20 s while
   the day passes; it draws the light, the fog and the rain or snow. */

typedef enum {
    SKY_CLEAR,
    SKY_RAIN,
    SKY_SNOW,
    SKY_FOG,
    SKY_STORM,
    SKY_WEATHER_COUNT
} sky_weather_t;

/* [sky] of config.toml (the 1.0 profile's) */
void sky_configure(toml_table_t* root);
/* the day passing: resends while it moves */
void sky_tick(server_t* server);
/* at every map start, after the mode's sky hook: owned = the mode set the
   sky. The map after a mode-owned one starts from config.toml's [sky]
   (an admin's /sky in a mode with no sky of its own keeps lasting) */
void sky_map_start(server_t* server, int owned);
/* a 1.0 client entering the game */
void sky_send_to(server_t* server, player_t* player);

/* minutes since midnight now, 0..1440 */
float       sky_minutes(void);
float       sky_rate(void);
int         sky_weather(void);
int         sky_intensity(void);
int         sky_moonlight(void);
const char* sky_weather_name(int weather);
/* -1 for a name that is no weather */
int sky_weather_find(const char* name);
/* minutes from "hh:mm", "day", "night", "dawn", "dusk", "noon" or
   "midnight"; -1 when it is none of them */
int sky_parse_time(const char* text);

/* each returns 0 when it changed nothing; the change goes to every 1.0
   client at once. `why` is logged (command, plugin, config) */
int sky_set_time(server_t* server, float minutes, const char* why);
int sky_set_rate(server_t* server, float rate, const char* why);
int sky_set_weather(server_t* server, int weather, int intensity, const char* why);
int sky_set_moonlight(server_t* server, int moonlight, const char* why);

/* how far a bot sees `target` by this light and weather: the server's fog
   by day and in clear weather (what it was); less by night and in rain,
   snow or fog, unless the target's flashlight is on or it fires, or the
   viewer has night vision (GM_RULE_NIGHT_VISION: the zombies) */
float sky_sight_range(server_t* server, player_t* viewer, const player_t* target, float fog);
/* dark enough for a lamp: from the evening to the morning */
int sky_dark(void);

#endif
