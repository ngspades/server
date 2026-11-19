// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Profile.h>
#include <Server/Proto10/Bridge.h>
#include <Server/Server.h>
#include <Server/Sky.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <sky/sky.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* while the day passes the clients run it on themselves; this resend only
   keeps their clocks from drifting off the server's (a client's frames, a
   server's SP_TIME_SCALE) */
#define SKY_RESEND (NANO_IN_SECOND * 20ULL)

static struct
{
    float    minutes; /* at `at` */
    float    rate;    /* minutes of the day a second of the game's clock */
    uint64_t at;
    uint8_t  weather;
    uint8_t  intensity; /* 0..100 */
    uint8_t  moonlight; /* 0..100 */
    uint64_t sent_at;
} sky = {.minutes = 720.0f, .intensity = 60, .moonlight = 25};

/* config.toml's [sky] as read (and the test overrides), and whether the
   current map's sky is a mode's (sky_map_start) */
static float configured_minutes, configured_rate;
static int   mode_owned;

static float _sky_wrap(float m)
{
    m = fmodf(m, GAME_SKY_DAY_MINUTES);
    return m < 0.0f ? m + GAME_SKY_DAY_MINUTES : m;
}

float sky_minutes(void)
{
    if (sky.rate == 0.0f) {
        return sky.minutes;
    }
    double seconds = (double) (get_nanos() - sky.at) / NANO_IN_SECOND;
    return _sky_wrap(sky.minutes + (float) (seconds * sky.rate));
}

float sky_rate(void)
{
    return sky.rate;
}

int sky_weather(void)
{
    return sky.weather;
}

int sky_intensity(void)
{
    return sky.intensity;
}

int sky_moonlight(void)
{
    return sky.moonlight;
}

const char* sky_weather_name(int weather)
{
    return game_sky_weather_name(weather);
}

int sky_weather_find(const char* name)
{
    return game_sky_weather_find(name);
}

int sky_parse_time(const char* text)
{
    static const struct
    {
        const char* name;
        int         minutes;
    } names[] = {{"day", 720}, {"noon", 720}, {"night", 0}, {"midnight", 0}, {"dawn", 360}, {"dusk", 1110}};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (!strcmp(text, names[i].name)) {
            return names[i].minutes;
        }
    }
    int  h, m;
    char tail;
    if (sscanf(text, "%d:%d%c", &h, &m, &tail) == 2 && h >= 0 && h <= 24 && m >= 0 && m < 60 && h * 60 + m <= 1440) {
        return (h * 60 + m) % 1440;
    }
    return -1;
}

static void _sky_send(server_t* server, player_t* to)
{
    if (profile_is_075()) {
        return; /* the 0.75 profile's day and night are Daytime and Weather's */
    }
    char minutes[16], rate[16], intensity[8], moon[8];
    snprintf(minutes, sizeof minutes, "%.1f", sky_minutes());
    snprintf(rate, sizeof rate, "%.3f", sky.rate);
    snprintf(intensity, sizeof intensity, "%u", (unsigned) sky.intensity);
    snprintf(moon, sizeof moon, "%u", (unsigned) sky.moonlight);
    const char* parts[6] = {"sky", minutes, rate, sky_weather_name(sky.weather), intensity, moon};
    proto10_bridge_generic(server, to, 6, parts);
    if (to == NULL) {
        sky.sent_at = get_nanos();
    }
}

static void _sky_changed(server_t* server, const char* what, const char* why)
{
    evt_log(NULL,
            "sky",
            "action=%s minutes=%.0f rate=%.3f weather=%s intensity=%u moonlight=%u why=%s",
            what,
            sky_minutes(),
            sky.rate,
            sky_weather_name(sky.weather),
            (unsigned) sky.intensity,
            (unsigned) sky.moonlight,
            why);
    if (server != NULL) {
        _sky_send(server, NULL);
    }
}

int sky_set_time(server_t* server, float minutes, const char* why)
{
    minutes = _sky_wrap(minutes);
    sky.minutes = minutes;
    sky.at      = get_nanos();
    _sky_changed(server, "time", why);
    return 1;
}

int sky_set_rate(server_t* server, float rate, const char* why)
{
    if (rate < 0.0f || rate > 1440.0f || rate == sky.rate) {
        return 0;
    }
    /* the time it is now becomes the new rate's start */
    sky.minutes = sky_minutes();
    sky.at      = get_nanos();
    sky.rate    = rate;
    _sky_changed(server, "rate", why);
    return 1;
}

int sky_set_weather(server_t* server, int weather, int intensity, const char* why)
{
    if (weather < 0 || weather >= SKY_WEATHER_COUNT || intensity < 0 || intensity > 100) {
        return 0;
    }
    if (weather == sky.weather && intensity == sky.intensity) {
        return 0;
    }
    sky.weather   = (uint8_t) weather;
    sky.intensity = (uint8_t) intensity;
    _sky_changed(server, "weather", why);
    return 1;
}

int sky_set_moonlight(server_t* server, int moonlight, const char* why)
{
    if (moonlight < 0 || moonlight > 100 || moonlight == sky.moonlight) {
        return 0;
    }
    sky.moonlight = (uint8_t) moonlight;
    _sky_changed(server, "moonlight", why);
    return 1;
}

static int _sky_int_in(toml_table_t* t, const char* key, int lo, int hi, int keep)
{
    toml_datum_t d = toml_int_in(t, key);
    if (!d.ok) {
        return keep;
    }
    if (d.u.i < lo || d.u.i > hi) {
        LOG_WARNING("[sky] %s must be %d..%d, keeping %d", key, lo, hi, keep);
        return keep;
    }
    return (int) d.u.i;
}

void sky_configure(toml_table_t* root)
{
    toml_table_t* t = root != NULL ? toml_table_in(root, "sky") : NULL;
    if (t != NULL) {
        toml_datum_t s = toml_string_in(t, "time");
        if (s.ok) {
            int m = sky_parse_time(s.u.s);
            if (m < 0) {
                LOG_WARNING("[sky] time '%s' is no time: \"hh:mm\", day, night, dawn or dusk", s.u.s);
            } else {
                sky.minutes = (float) m;
            }
            free(s.u.s);
        }
        toml_datum_t r = toml_double_in(t, "rate");
        if (!r.ok) {
            r = toml_int_in(t, "rate");
            r.u.d = r.ok ? (double) r.u.i : 0.0;
        }
        if (r.ok && r.u.d >= 0.0 && r.u.d <= 1440.0) {
            sky.rate = (float) r.u.d;
        } else if (r.ok) {
            LOG_WARNING("[sky] rate must be 0..1440 minutes a second");
        }
        s = toml_string_in(t, "weather");
        if (s.ok) {
            int w = sky_weather_find(s.u.s);
            if (w < 0) {
                LOG_WARNING("[sky] weather '%s' is none of clear, rain, snow, fog, storm", s.u.s);
            } else {
                sky.weather = (uint8_t) w;
            }
            free(s.u.s);
        }
        sky.intensity = (uint8_t) _sky_int_in(t, "intensity", 0, 100, sky.intensity);
        sky.moonlight = (uint8_t) _sky_int_in(t, "moonlight", 0, 100, sky.moonlight);
    }
    /* tests */
    const char* e = getenv("SP_SKY_TIME");
    if (e != NULL && *e && sky_parse_time(e) >= 0) {
        sky.minutes = (float) sky_parse_time(e);
    }
    e = getenv("SP_SKY_WEATHER");
    if (e != NULL && *e && sky_weather_find(e) >= 0) {
        sky.weather = (uint8_t) sky_weather_find(e);
    }
    e = getenv("SP_SKY_RATE");
    if (e != NULL && *e) {
        float rate = strtof(e, NULL);
        sky.rate   = rate >= 0.0f && rate <= 1440.0f ? rate : sky.rate;
    }
    sky.at             = get_nanos();
    configured_minutes = sky.minutes;
    configured_rate    = sky.rate;
    if (!profile_is_075()) {
        LOG_INFO("Sky: %02d:%02d, %.2f minutes a second, %s %u%%, moonlight %u%%",
                 (int) sky.minutes / 60,
                 (int) sky.minutes % 60,
                 sky.rate,
                 sky_weather_name(sky.weather),
                 (unsigned) sky.intensity,
                 (unsigned) sky.moonlight);
    }
}

void sky_tick(server_t* server)
{
    if (sky.rate == 0.0f || profile_is_075()) {
        return;
    }
    uint64_t now = get_nanos();
    if (now - sky.sent_at >= SKY_RESEND) {
        _sky_send(server, NULL);
    }
}

void sky_send_to(server_t* server, player_t* player)
{
    _sky_send(server, player);
}

int sky_dark(void)
{
    /* from the evening: a lamp is on before the night is black */
    return !profile_is_075() && game_sky_daylight(sky_minutes()) < 0.6f;
}

float sky_sight_range(server_t* server, player_t* viewer, const player_t* target, float fog)
{
    /* by day in clear weather the fog exactly, as it always was: a bot's
       sight must not move by a rounding where nothing changed */
    float minutes = sky_minutes();
    if (sky.weather == SKY_CLEAR && game_sky_daylight(minutes) >= 1.0f) {
        return fog;
    }
    /* a lamp or a muzzle flash shows its owner as far as anything shows;
       night vision sees the dark as day (the weather still hides) */
    if (target->light_on || target->primary_fire) {
        return fog;
    }
    if (gamemode_player_rules(server, viewer) & GM_RULE_NIGHT_VISION) {
        return sky.weather == SKY_CLEAR ? fog : fog * game_sky_visibility(sky.weather, sky.intensity / 100.0f);
    }
    return game_sky_sight(fog, minutes, sky.weather, sky.intensity / 100.0f, (float) sky.moonlight);
}

void sky_map_start(server_t* server, int owned)
{
    if (owned) {
        mode_owned = 1;
        return;
    }
    if (mode_owned) {
        mode_owned = 0;
        sky_set_rate(server, configured_rate, "config");
        sky_set_time(server, configured_minutes, "config");
    }
}
