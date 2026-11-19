// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Commands/Commands.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Server/Sky.h>
#include <Util/Notice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* /sky: the time of day and the weather of the 1.0 profile (Sky.c).
     /sky                               what it is now
     /sky time <hh:mm|day|night|dawn|dusk>
     /sky rate <minutes a second>       0 stops the clock
     /sky weather <clear|rain|snow|fog|storm> [intensity 0..100]
     /sky moon <0..100>                 how much a night shows */
void cmd_sky(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    char      line[160];
    if (profile_is_075()) {
        send_server_notice(arguments.player, arguments.console, "The 0.75 profile has /night (Daytime and Weather)");
        return;
    }
    if (arguments.argc == 1) {
        float m = sky_minutes();
        snprintf(line,
                 sizeof line,
                 "%02d:%02d, %.2f minutes a second, %s %d%%, moonlight %d%%",
                 (int) m / 60,
                 (int) m % 60,
                 sky_rate(),
                 sky_weather_name(sky_weather()),
                 sky_intensity(),
                 sky_moonlight());
        send_server_notice(arguments.player, arguments.console, line);
        return;
    }
    const char* what = arguments.argv[1];
    const char* a    = arguments.argc > 2 ? arguments.argv[2] : NULL;
    if (!strcmp(what, "time") && a != NULL && arguments.argc == 3) {
        int m = sky_parse_time(a);
        if (m < 0) {
            send_server_notice(arguments.player, arguments.console, "A time is hh:mm, day, night, dawn or dusk");
            return;
        }
        sky_set_time(server, (float) m, "command");
        snprintf(line, sizeof line, "It is %02d:%02d", m / 60, m % 60);
        send_server_notice(arguments.player, arguments.console, line);
        return;
    }
    if (!strcmp(what, "rate") && a != NULL && arguments.argc == 3) {
        char* end;
        float rate = strtof(a, &end);
        if (*end || rate < 0.0f || rate > 1440.0f) {
            send_server_notice(arguments.player, arguments.console, "A rate is 0..1440 minutes of the day a second");
            return;
        }
        sky_set_rate(server, rate, "command");
        send_server_notice(arguments.player, arguments.console, rate == 0.0f ? "The time stands" : "The time passes");
        return;
    }
    if (!strcmp(what, "weather") && a != NULL && arguments.argc <= 4) {
        int w = sky_weather_find(a);
        int i = arguments.argc == 4 ? atoi(arguments.argv[3]) : sky_intensity();
        if (w < 0 || i < 0 || i > 100) {
            send_server_notice(arguments.player, arguments.console, "Usage: /sky weather <clear|rain|snow|fog|storm> [0..100]");
            return;
        }
        sky_set_weather(server, w, i, "command");
        snprintf(line, sizeof line, "Weather: %s %d%%", sky_weather_name(w), i);
        send_server_notice(arguments.player, arguments.console, line);
        return;
    }
    if (!strcmp(what, "moon") && a != NULL && arguments.argc == 3) {
        int m = atoi(a);
        if (m < 0 || m > 100) {
            send_server_notice(arguments.player, arguments.console, "Moonlight is 0..100");
            return;
        }
        sky_set_moonlight(server, m, "command");
        snprintf(line, sizeof line, "Moonlight: %d%%", m);
        send_server_notice(arguments.player, arguments.console, line);
        return;
    }
    send_server_notice(arguments.player,
                       arguments.console,
                       "Usage: /sky [time <hh:mm|day|night|dawn|dusk> | rate <n> | weather <kind> [0..100] | moon <0..100>]");
}
