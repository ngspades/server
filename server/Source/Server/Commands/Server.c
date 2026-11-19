// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Commands/CommandManager.h>
#include <Server/Profile.h>
#include <Server/Server.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/DataDir.h>
#include <Util/Notice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc99/toml.h>
#include <unistd.h>

void cmd_server(void* p_server, command_args_t arguments)
{
    (void) p_server;
    send_server_notice(arguments.player, arguments.console, "You are playing on %s. Version %s", GAME_SERVER_TITLE, VERSION);
}

/* a path the server opens relative to its data root, made absolute so the
   banner says which file it is wherever the server was started from */
static void _profile_absolute(char* out, size_t size, const char* path)
{
    char cwd[768];
    if (path[0] == '/' || getcwd(cwd, sizeof cwd) == NULL) {
        snprintf(out, size, "%s", path);
        return;
    }
    if (!strcmp(path, ".")) {
        snprintf(out, size, "%s", cwd);
        return;
    }
    snprintf(out, size, "%s/%s", cwd, strncmp(path, "./", 2) == 0 ? path + 2 : path);
}

/* The facts are read again from the config the way their owners read them
   (Net/WebRtc.c's [webrtc] port, Net/Master.c's [master] enabled and url,
   Legacy/Master075.c's [master075]), environment overrides included, so the
   banner and /profile say what the server does, not what a default says. */
int profile_report_lines(uint16_t port, int max_players, char lines[][PROFILE_REPORT_LINE], int max)
{
    long webrtc_port   = (long) port + 1;
    int  webrtc_on     = 0; /* Net/WebRtc.c's rule: [webrtc] enabled, or a join code */
    int  our_master    = 1;
    int  our_url       = 1;
    int  public_master = 1;
    char public_addr[128];
    snprintf(public_addr, sizeof public_addr, "67.205.183.163:32886");

    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "webrtc") : NULL;
        if (t != NULL) {
            toml_datum_t d = toml_int_in(t, "port");
            if (d.ok && d.u.i > 0) {
                webrtc_port = (long) d.u.i;
            }
            d = toml_bool_in(t, "enabled");
            if (d.ok) {
                webrtc_on = d.u.b;
            }
        }
        t = root != NULL ? toml_table_in(root, "master") : NULL;
        if (t != NULL) {
            toml_datum_t d = toml_bool_in(t, "enabled");
            if (d.ok) {
                our_master = d.u.b;
            }
            d = toml_string_in(t, "url");
            if (d.ok) {
                our_url = d.u.s[0] != '\0';
                free(d.u.s);
            }
        }
        t = root != NULL ? toml_table_in(root, "master075") : NULL;
        if (t != NULL) {
            toml_datum_t d = toml_bool_in(t, "enabled");
            if (d.ok) {
                public_master = d.u.b;
            }
            d = toml_string_in(t, "address");
            if (d.ok) {
                snprintf(public_addr, sizeof public_addr, "%s", d.u.s);
                free(d.u.s);
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    const char* env = getenv("SP_WEBRTC_PORT");
    if (env != NULL && *env != '\0') {
        webrtc_port = strtol(env, NULL, 10);
    }
    env = getenv("SP_WEBRTC_ENABLED");
    if (env != NULL && *env != '\0') {
        webrtc_on = atoi(env) != 0;
    }
    if (get_server()->join_code[0]) {
        webrtc_on = 1;
    }
    env = getenv("SP_MASTER_URL");
    if (env != NULL && *env != '\0') {
        our_master = 1;
        our_url    = 1;
    }
    /* SP_MASTER=0 keeps every test server off every list, the public ones too */
    env = getenv("SP_MASTER");
    if (env != NULL && !strcmp(env, "0")) {
        our_master    = 0;
        public_master = 0;
    }
    if (get_server()->listen_private) {
        our_master    = 0;
        public_master = 0;
    }
    our_master = our_master && our_url;

    char path[900];
    int  n = 0;
    if (n < max) {
        snprintf(lines[n++], PROFILE_REPORT_LINE, "Profile: %s (--profile %s)",
                 profile_is_075() ? "0.75 only (classic CTF)" : "1.0 only", server_profile_name());
    }
    if (n < max) {
        _profile_absolute(path, sizeof path, server_config_path());
        snprintf(lines[n++], PROFILE_REPORT_LINE, "Config: %s", path);
    }
    if (n < max) {
        _profile_absolute(path, sizeof path, *data_dir() != '\0' ? data_dir() : ".");
        snprintf(lines[n++], PROFILE_REPORT_LINE, "Data: %s", path);
    }
    if (n < max) {
        char browsers[64] = "no browsers";
        if (webrtc_on) {
            snprintf(browsers, sizeof browsers, "WebRTC %ld", webrtc_port);
        }
        snprintf(lines[n++], PROFILE_REPORT_LINE, "Port: %u (%s), %d players at most", (unsigned) port, browsers,
                 max_players);
    }
    if (n < max) {
        if (profile_is_075()) {
            snprintf(lines[n++], PROFILE_REPORT_LINE, "Master: public 0.75 masters %s (%s), our master %s",
                     public_master ? "on" : "off", public_addr, our_master ? "on" : "off");
        } else {
            snprintf(lines[n++], PROFILE_REPORT_LINE, "Master: our master %s", our_master ? "on" : "off");
        }
    }
    return n;
}

/* "which server am I talking to?": the banner's lines, at runtime */
void cmd_profile(void* p_server, command_args_t arguments)
{
    server_t* server = (server_t*) p_server;
    char      lines[PROFILE_REPORT_MAX][PROFILE_REPORT_LINE];
    int       n = profile_report_lines(server->port, server->protocol.max_players, lines, PROFILE_REPORT_MAX);
    for (int i = 0; i < n; i++) {
        send_server_notice(arguments.player, arguments.console, "%s", lines[i]);
    }
}
