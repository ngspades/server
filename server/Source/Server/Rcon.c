// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Audit.h>
#include <Server/Bots.h>
#include <Server/Classes.h>
#include <Server/Commands/CommandManager.h>
#include <Server/Commands/Commands.h>
#include <Server/EventLog.h>
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Player.h>
#include <Server/Profile.h>
#include <Server/Rcon.h>
#include <Server/Structs/PlayerStruct.h>
#include <Util/Enums.h>
#include <Util/Log.h>
#include <Util/Nanos.h>
#include <Util/Notice.h>
#include <uthash/uthash.h>
#include <json-c/json.h>
#include <tomlc99/toml.h>

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

void rcon_start(server_t* server)
{
    (void) server;
}
void rcon_tick(server_t* server)
{
    (void) server;
}
void rcon_stop(void) {}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RCON_DEFAULT_PORT 32890
#define RCON_LINE         1000
#define RCON_WAIT_SECONDS 5
#define RCON_OUTPUT       65536
#define RCON_MIN_PASSWORD 16
#define RCON_EOT          '\x04'

static struct
{
    int             on;
    char            bind[64];
    int             port;
    char            password[128];
    int             listen_fd;
    pthread_t       thread;
    server_t*       server;
    /* the one line waiting for the tick: the thread serves one client at a
       time */
    pthread_mutex_t lock;
    pthread_cond_t  done_cond;
    int             pending;
    int             done;
    char            line[RCON_LINE + 2];
    char            out[RCON_OUTPUT];
} rcon = {.listen_fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .done_cond = PTHREAD_COND_INITIALIZER};

/* --- on the tick --- */

static const char* _team_name(uint8_t team)
{
    return team == TEAM_A ? "1" : team == TEAM_B ? "2" : "spectator";
}

/* the whole picture in one object: what a monitor or sadctl shows */
static void _status(server_t* server, char* out, size_t size)
{
    json_object* o = json_object_new_object();
    json_object_object_add(o, "name", json_object_new_string(server->server_name));
    json_object_object_add(o, "map", json_object_new_string(server->map_name));
    json_object_object_add(o, "mode", json_object_new_string(gamemode_current(server)->name));
    json_object_object_add(o, "profile", json_object_new_string(profile_is_075() ? "075" : "10"));
    json_object_object_add(o, "uptime_seconds",
                           json_object_new_int64((int64_t) ((get_nanos() - server->match_started_at) / NANO_IN_SECOND)));
    json_object_object_add(o, "max_players", json_object_new_int(server->protocol.max_players));
    json_object_object_add(o, "port", json_object_new_int(server->port));
    json_object_object_add(o, "classes", json_object_new_boolean(classes_on(server)));
    json_object_object_add(o, "bot_difficulty", json_object_new_string(bot_difficulty_name(bot_difficulty())));
    json_object* score = json_object_new_array();
    json_object_array_add(score, json_object_new_int(server->protocol.gamemode.score[0]));
    json_object_array_add(score, json_object_new_int(server->protocol.gamemode.score[1]));
    json_object_object_add(o, "score", score);
    json_object_object_add(o, "score_limit", json_object_new_int(server->protocol.gamemode.score_limit));
    json_object* players = json_object_new_array();
    int          humans = 0, bots = 0;
    player_t *   p, *tmp;
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p->state == STATE_DISCONNECTED) {
            continue;
        }
        p->is_bot ? bots++ : humans++;
        json_object* q = json_object_new_object();
        json_object_object_add(q, "id", json_object_new_int(p->id));
        json_object_object_add(q, "name", json_object_new_string(p->name));
        json_object_object_add(q, "team", json_object_new_string(_team_name(p->team)));
        json_object_object_add(q, "bot", json_object_new_boolean(p->is_bot));
        json_object_object_add(q, "alive", json_object_new_boolean(p->alive));
        json_object_object_add(q, "hp", json_object_new_int(p->hp));
        json_object_object_add(q, "kills", json_object_new_int(p->kills));
        json_object_object_add(q, "deaths", json_object_new_int(p->deaths));
        json_object_object_add(q, "assists", json_object_new_int(p->assists));
        json_object_object_add(q, "captures", json_object_new_int(p->captures));
        json_object_object_add(q, "score", json_object_new_int(p->kills + p->score));
        if (!p->is_bot) {
            char ip[64];
            player_address(p, ip, sizeof ip);
            json_object_object_add(q, "ip", json_object_new_string(ip));
            json_object_object_add(q, "ping_ms", json_object_new_int(p->conn != NULL ? (int) conn_rtt(p->conn) : 0));
        }
        json_object_array_add(players, q);
    }
    json_object_object_add(o, "humans", json_object_new_int(humans));
    json_object_object_add(o, "bots", json_object_new_int(bots));
    json_object_object_add(o, "players", players);
    snprintf(out, size, "%s", json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    json_object_put(o);
}

/* a console command, as typed at the server's console; what it prints for
   the console is the answer */
static void _run_line(server_t* server, const char* text, char* out, size_t size)
{
    char line[RCON_LINE + 2];
    snprintf(line, sizeof line, "%s%s", text[0] == '/' ? "" : "/", text);
    char       first[64] = {0};
    command_t* cmd       = NULL;
    sscanf(line, "%63s", first);
    for (char* k = first; *k; k++) {
        *k = (char) tolower((unsigned char) *k);
    }
    HASH_FIND_STR(server->cmds_map, first, cmd);
    if (cmd == NULL) {
        snprintf(out, size, "no such command: /help lists them");
        return;
    }
    /* what ends the server or the login is not for a remote tool */
    if (!strcmp(first, "/shutdown") || !strcmp(first, "/login") || !strcmp(first, "/logout")) {
        snprintf(out, size, "not from here");
        return;
    }
    out[0] = '\0';
    notice_capture(out, size);
    command_handle(server, NULL, line, 1);
    notice_capture(NULL, 0);
}

void rcon_tick(server_t* server)
{
    pthread_mutex_lock(&rcon.lock);
    if (rcon.pending && !rcon.done) {
        int is_status = !strcmp(rcon.line, "status");
        if (is_status) {
            _status(server, rcon.out, sizeof rcon.out);
        } else {
            _run_line(server, rcon.line, rcon.out, sizeof rcon.out);
            audit_event(NULL, 0, "rcon", "line=%s", rcon.line);
        }
        rcon.done = 1;
        pthread_cond_signal(&rcon.done_cond);
    }
    pthread_mutex_unlock(&rcon.lock);
}

/* --- the socket --- */

/* hands a line to the tick and waits for its answer (0: no answer in time) */
static int _call_on_tick(const char* line)
{
    pthread_mutex_lock(&rcon.lock);
    snprintf(rcon.line, sizeof rcon.line, "%s", line);
    rcon.done    = 0;
    rcon.pending = 1;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += RCON_WAIT_SECONDS;
    int rc = 0;
    while (!rcon.done && rc == 0) {
        rc = pthread_cond_timedwait(&rcon.done_cond, &rcon.lock, &until);
    }
    int ok       = rcon.done;
    rcon.pending = 0;
    pthread_mutex_unlock(&rcon.lock);
    return ok;
}

static int _send_all(int fd, const char* data, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, data, n, 0);
        if (w <= 0) {
            return 0;
        }
        data += w;
        n -= (size_t) w;
    }
    return 1;
}

static int _reply(int fd, const char* text)
{
    static const char end[] = {RCON_EOT, '\n'};
    return _send_all(fd, text, strlen(text)) && _send_all(fd, end, sizeof end);
}

/* the password compared in time that does not depend on where it differs */
static int _password_ok(const char* given)
{
    size_t a = strlen(given), b = strlen(rcon.password);
    unsigned char diff = (unsigned char) (a != b);
    for (size_t i = 0; i < b; i++) {
        diff |= (unsigned char) (given[i < a ? i : 0] ^ rcon.password[i]);
    }
    return diff == 0;
}

/* a line from the client, without its newline; 0 at the end, on a long one,
   when the server stops or after a quiet minute. The socket times out every
   second so a connected tool never holds the server's shutdown up */
static int _read_line(int fd, char* line, size_t size)
{
    size_t n    = 0;
    int    idle = 0;
    for (;;) {
        char    c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!rcon.server->running || ++idle >= 60) {
                return 0;
            }
            continue;
        }
        if (r <= 0) {
            return 0;
        }
        idle = 0;
        if (c == '\n') {
            line[n] = '\0';
            if (n > 0 && line[n - 1] == '\r') {
                line[n - 1] = '\0';
            }
            return 1;
        }
        if (n + 1 >= size) {
            return 0;
        }
        line[n++] = c;
    }
}

static void _serve(int fd)
{
    char line[RCON_LINE + 2];
    if (!_read_line(fd, line, sizeof line) || strncmp(line, "auth ", 5) != 0 || !_password_ok(line + 5)) {
        /* a wrong guess costs a second: a password is not for guessing */
        sleep(1);
        _reply(fd, "denied");
        evt_log(NULL, "rcon", "action=refused why=auth");
        return;
    }
    if (!_reply(fd, "ok")) {
        return;
    }
    while (_read_line(fd, line, sizeof line)) {
        if (!*line) {
            continue;
        }
        if (!_call_on_tick(line)) {
            if (!_reply(fd, "the server did not answer in time")) {
                return;
            }
            continue;
        }
        if (!_reply(fd, rcon.out[0] ? rcon.out : "(done, nothing printed)")) {
            return;
        }
    }
}

static void* _thread(void* arg)
{
    (void) arg;
    while (rcon.server->running) {
        fd_set         set;
        struct timeval wait = {0, 250000};
        FD_ZERO(&set);
        FD_SET(rcon.listen_fd, &set);
        if (select(rcon.listen_fd + 1, &set, NULL, NULL, &wait) <= 0) {
            continue;
        }
        int fd = accept(rcon.listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        /* the receive times out every second (see _read_line); a client that
           stops reading cannot hold the thread either */
        struct timeval limit = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
        _serve(fd);
        close(fd);
    }
    close(rcon.listen_fd);
    rcon.listen_fd = -1;
    return NULL;
}

static void _read_config(void)
{
    rcon.port = RCON_DEFAULT_PORT;
    snprintf(rcon.bind, sizeof rcon.bind, "127.0.0.1");
    FILE* fp = fopen(server_config_path(), "r");
    if (fp != NULL) {
        char          err[200];
        toml_table_t* root = toml_parse_file(fp, err, sizeof err);
        fclose(fp);
        toml_table_t* t = root != NULL ? toml_table_in(root, "rcon") : NULL;
        if (t != NULL) {
            int     on = 0;
            int64_t port;
            char*   s = NULL;
            if (toml_raw_in(t, "enabled") != NULL && toml_rtob(toml_raw_in(t, "enabled"), &on) == 0) {
                rcon.on = on;
            }
            if (toml_raw_in(t, "port") != NULL && toml_rtoi(toml_raw_in(t, "port"), &port) == 0) {
                rcon.port = (int) port;
            }
            if (toml_raw_in(t, "bind") != NULL && toml_rtos(toml_raw_in(t, "bind"), &s) == 0) {
                snprintf(rcon.bind, sizeof rcon.bind, "%s", s);
                free(s);
                s = NULL;
            }
            if (toml_raw_in(t, "password") != NULL && toml_rtos(toml_raw_in(t, "password"), &s) == 0) {
                snprintf(rcon.password, sizeof rcon.password, "%s", s);
                free(s);
            }
        }
        if (root != NULL) {
            toml_free(root);
        }
    }
    /* tests and containers: the environment wins, and a password there turns it on */
    if (getenv("SP_RCON_PASSWORD") != NULL) {
        snprintf(rcon.password, sizeof rcon.password, "%s", getenv("SP_RCON_PASSWORD"));
        rcon.on = 1;
    }
    if (getenv("SP_RCON_PORT") != NULL) {
        rcon.port = atoi(getenv("SP_RCON_PORT"));
    }
    if (getenv("SP_RCON_BIND") != NULL) {
        snprintf(rcon.bind, sizeof rcon.bind, "%s", getenv("SP_RCON_BIND"));
    }
}

void rcon_start(server_t* server)
{
    rcon.server = server;
    _read_config();
    if (!rcon.on) {
        return;
    }
    if (strlen(rcon.password) < RCON_MIN_PASSWORD) {
        LOG_WARNING("RCON: off, its password must be %d characters or more ([rcon] password in config.toml)",
                    RCON_MIN_PASSWORD);
        evt_log(NULL, "rcon", "action=refused why=short_password");
        return;
    }
    rcon.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes        = 1;
    setsockopt(rcon.listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t) rcon.port);
    if (inet_pton(AF_INET, rcon.bind, &addr.sin_addr) != 1 ||
        bind(rcon.listen_fd, (struct sockaddr*) &addr, sizeof addr) != 0 || listen(rcon.listen_fd, 4) != 0)
    {
        LOG_ERROR("RCON: cannot listen on %s:%d (%s)", rcon.bind, rcon.port, strerror(errno));
        close(rcon.listen_fd);
        rcon.listen_fd = -1;
        return;
    }
    if (pthread_create(&rcon.thread, NULL, _thread, NULL) != 0) {
        LOG_ERROR("RCON: no thread");
        close(rcon.listen_fd);
        rcon.listen_fd = -1;
        return;
    }
    LOG_INFO("RCON: remote console on %s:%d", rcon.bind, rcon.port);
    if (strcmp(rcon.bind, "127.0.0.1") != 0) {
        LOG_WARNING("RCON: listening beyond this machine (%s): the password travels in the clear", rcon.bind);
    }
    evt_log(NULL, "rcon", "action=start bind=%s port=%d", rcon.bind, rcon.port);
}

void rcon_stop(void)
{
    if (rcon.listen_fd >= 0) {
        pthread_join(rcon.thread, NULL);
    }
}

#endif
