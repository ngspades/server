// Copyright DarkNeutrino 2021
// Copyright (c) 2026 ngspades contributors
#include <Server/Bots.h>
#include <Server/Commands/CommandManager.h>
#include <Server/Legacy/Ext075.h>
#include <Server/Legacy/Master075.h>
#include <Server/Profile.h>
#include <Server/ConfigCheck.h>
#include <Server/Server.h>
#include <Server/Sky.h>
#include <Server/Structs/ServerStruct.h>
#include <Server/Structs/StartStruct.h>
#include <Util/DataDir.h>
#include <Util/JSONHelpers.h>
#include <Util/TOMLHelpers.h>
#include <Util/Log.h>
#include <Util/Types.h>
#include <uthash/utlist.h>
#include <Util/Alloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <tomlc99/toml.h>

#ifdef _WIN32
#include <direct.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define HAVE_EXECINFO 1
#endif
#endif
#include <Server/Gamemodes/Gamemode.h>
#include <Server/Plugins.h>
#include <Server/Map.h>
#include <Server/MapDownload.h>
#include <modes/modes.h>

/* The server keeps its data (config.toml, maps) inside a single root folder.
   Default resolution order: --root <dir>, the current directory, then a few
   parent directories, so the binary can be started from build/ or from the
   project root without copying anything around. */
/* "{game}" in config.toml text becomes GAME_SERVER_TITLE (common/game.cmake), so the
   server name and messages follow the one place the name is set. Always
   returns a new heap string; frees the input when the caller owns it. */
static char* expand_game_name(char* text, int owned)
{
    static const char token[] = "{game}";
    size_t            count   = 0;
    for (const char* at = strstr(text, token); at != NULL; at = strstr(at + 1, token)) {
        count++;
    }
    size_t len = strlen(text) + count * (strlen(GAME_SERVER_TITLE) - (sizeof token - 1)) + 1;
    char*  out = spadesx_malloc(len);
    char*  w   = out;
    for (const char* r = text; *r != 0;) {
        if (strncmp(r, token, sizeof token - 1) == 0) {
            memcpy(w, GAME_SERVER_TITLE, strlen(GAME_SERVER_TITLE));
            w += strlen(GAME_SERVER_TITLE);
            r += sizeof token - 1;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    if (owned) {
        free(text);
    }
    return out;
}

static void free_maps(string_node_t** list, size_t* count)
{
    string_node_t *node, *tmp;
    DL_FOREACH_SAFE(*list, node, tmp)
    {
        DL_DELETE(*list, node);
        free(node->string);
        free(node);
    }
    *count = 0;
}

/* a preset's maps replace the server's rotation */
static void maps_from_table(toml_table_t* t, const char* key, string_node_t** list, size_t* count)
{
    toml_array_t* a = toml_array_in(t, key);
    if (a == NULL) {
        return;
    }
    free_maps(list, count);
    for (int k = 0; k < toml_array_nelem(a); k++) {
        toml_datum_t d = toml_string_at(a, k);
        if (!d.ok) {
            continue;
        }
        string_node_t* node = spadesx_malloc(sizeof(string_node_t));
        node->string        = strdup(d.u.s);
        free(d.u.s);
        DL_APPEND(*list, node);
        (*count)++;
    }
}

static const char* resolve_data_root(int argc, char** argv)
{
    static char root[512];

    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--root")) {
            snprintf(root, sizeof(root), "%s", argv[i + 1]);
            return root;
        }
    }

    /* either profile's file marks a root: a root holding only one of them
       is still found, and a missing one is reported by name below */
    const char* tries[] = {".", "..", "../.."};
    const char* marks[] = {"Resources/config.toml", "Resources/config-075.toml"};
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        for (size_t m = 0; m < sizeof(marks) / sizeof(marks[0]); m++) {
            snprintf(root, sizeof(root), "%s/%s", tries[i], marks[m]);
            struct stat st;
            if (stat(root, &st) == 0) {
                snprintf(root, sizeof(root), "%s", tries[i]);
                return root;
            }
        }
    }
    return NULL;
}

/* The 0.75 profile's own data folder, inside the root: its Bans.json,
   master.key (its own identity on our master), webrtc.pem, logs/ and the
   plugins' stores never mix with the 1.0 server's, which writes in the root
   itself (an empty data_dir). "The default plus -075" is this: the 1.0
   default is the root, so the 0.75 one is the root's data-075/ (git-ignored). */
#define PROFILE_075_DATA_DIR "data-075"

/* --data <dir>, else SP_DATA_DIR: where the server writes (Util/DataDir.h);
   an explicit one wins in both profiles. Else NULL keeps the root (1.0) or
   PROFILE_075_DATA_DIR (0.75) */
static const char* resolve_data_dir(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--data")) {
            return argv[i + 1];
        }
    }
    const char* env = getenv("SP_DATA_DIR");
    if (env != NULL && *env != '\0') {
        return env;
    }
    return profile_is_075() ? PROFILE_075_DATA_DIR : NULL;
}

/* The maps of a rotation that are not installed come from the maps
   repository (MapDownload.c: before the first map loads, never on the tick);
   one still missing leaves the rotation, so a typo or a map that is not in
   the repository does not stop the server while others can play */
static void install_rotation(string_node_t** list, size_t* count)
{
    string_node_t *node, *tmp;
    DL_FOREACH_SAFE(*list, node, tmp)
    {
        /* a name the host wrote the way a 0.75 server or aos.party has it:
           the folder it means replaces it, so the map's own name is what
           loads and what the master and the clients see */
        char folder[MAP_NAME_MAX + 1];
        if (map_resolve_alias(node->string, folder, sizeof folder, 1)) {
            char* copy = strdup(folder);
            if (copy != NULL) {
                free(node->string);
                node->string = copy;
            }
        }
        if (map_download_now(node->string)) {
            continue;
        }
        LOG_WARNING("Map '%s' is not installed%s: left out of the rotation", node->string,
                    map_download_enabled() ? " and could not be downloaded" : " ([maps] download is off)");
        DL_DELETE(*list, node);
        free(node->string);
        free(node);
        (*count)--;
    }
}

/* a password still the shipped one is everyone's: anybody who read the
   default config.toml could log in as the manager */
static void warn_default_passwords(const char* const passwords[5])
{
    static const char* const roles[] = {"manager", "admin", "moderator", "guard", "trusted"};
    int                      shipped = 0;
    for (int k = 0; k < 5; k++) {
        if (passwords[k] != NULL && !strncmp(passwords[k], "changeme", 8)) {
            shipped++;
        }
    }
    if (shipped == 0) {
        return;
    }
    LOG_WARNING("****************************************************************");
    for (int k = 0; k < 5; k++) {
        if (passwords[k] != NULL && !strncmp(passwords[k], "changeme", 8)) {
            LOG_WARNING("The %s password is still the default '%s': anyone can log in with it.", roles[k],
                        passwords[k]);
        }
    }
    LOG_WARNING("Change them in %s, [passwords], then restart the server.", server_config_path());
    LOG_WARNING("****************************************************************");
}

/* the modes the 0.75 profile plays: the classic game's (plan 3.1), plus a
   Lua plugin mode (Resources/plugins/<name>.lua) that declares itself for
   the 0.75 profile through game.supported_profiles("075"). The real check
   of that declaration happens when the plugin is loaded (profile_refused);
   here we only let the name through so --gamemode can pick it. */
static int profile_075_mode(const char* name)
{
    if (!strcmp(name, "ctf") || !strcmp(name, "tc") || !strcmp(name, "babel")) {
        return 1;
    }
    return plugins_exists(name);
}

static void print_help(void)
{
    printf("Usage: %s [options]\n"
           "\n"
           "Profile and files (read before the config):\n"
           "  --profile <10|075>   10: protocol 1.0 only (the default); 075: protocol 0.75 only,\n"
           "                       the classic game (also SP_PROFILE)\n"
           "  --config <path>      the config file (default: Resources/config.toml, or\n"
           "                       Resources/config-075.toml with --profile 075)\n"
           "  --root <dir>         the data root holding Resources/ (default: ., .. or ../..)\n"
           "  --data <dir>         where the server writes: bans, keys, logs, plugin stores\n"
           "                       (also SP_DATA_DIR; default: the root, or data-075/ in it\n"
           "                       with --profile 075)\n"
           "  --check-config       validate the profile's config file and exit\n"
           "\n"
           "Game (override the config):\n"
           "  --port <n>           the game's UDP port\n"
           "  --preset <name>      a [presets.<name>] table of the config (also SP_PRESET)\n"
           "  --gamemode <name>    ctf, ngctf, babel, arena, zombie, creative or a Lua mode\n"
           "                       (--profile 075: ctf or babel)\n"
           "  --map <Name>         play this map of Resources/maps/ instead of the rotation\n"
           "  --maps-dir <dir>     a folder of maps searched before Resources/maps/\n"
           "  --classes <0|1>      the zombie mode's classes off or on\n"
           "  --first-infected <n> the zombie mode's patient zeros a round (0 or auto: one per 6 players)\n"
           "  --bots <n>           start with n balanced bots\n"
           "  --team-bots <a> <b>  start with a bots on team 1 and b on team 2\n"
           "  --bots-test          start with exactly one strong enemy bot\n"
           "\n"
           "Listen server (started by the client):\n"
           "  --listen             a player's own match: private, the host owns it\n"
           "  --server             the client's dedicated server, listed\n"
           "  --public             list a --listen match on the server list\n"
           "\n"
           "  --help               this text\n",
           GAME_SERVER_BINARY);
}

#if defined(HAVE_EXECINFO)
/* the handler may not format a path: made once at start-up */
static char crash_log_path[1100] = "logs/crash.log";

/* A crash leaves the stack in logs/crash.log (and on stderr) before the
   process dies. The server used to leave nothing but "Segmentation fault",
   which cost an afternoon per report. */
static void _crash_handler(int sig)
{
    static volatile sig_atomic_t in_handler;
    if (in_handler) {
        _exit(128 + sig);
    }
    in_handler = 1;
    void* frames[64];
    int   n = backtrace(frames, 64);
    int   fd = open(crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf(fd, "signal %d at %ld\n", sig, (long) time(NULL));
        backtrace_symbols_fd(frames, n, fd);
        close(fd);
    }
    dprintf(STDERR_FILENO, "signal %d: stack in %s\n", sig, crash_log_path);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

#include <server_default_configs.h>

/* The profile's own config (never a --config path) is written from the
   copy built in when the root has none: an empty folder given as --root (a
   Docker bind mount, the scratch image's volume) then starts like an
   unpacked release, and a release folder gets the other profile's config.
   Each profile writes only its own, so the 0.75 server still never reads
   config.toml and takes the 1.0 port. */
static void _write_default_config(void)
{
    const char* path = server_config_path();
    const char* text = NULL;
    if (!strcmp(path, "Resources/config.toml")) {
        text = default_config;
    } else if (!strcmp(path, "Resources/config-075.toml")) {
        text = default_config_075;
    }
    if (text == NULL) {
        return;
    }
#ifdef _WIN32
    _mkdir("Resources");
#else
    mkdir("Resources", 0755);
#endif
    FILE* fp = fopen(path, "wb");
    if (fp == NULL) {
        return;
    }
    size_t length = strlen(text);
    int    ok = fwrite(text, 1, length, fp) == length;
    if (fclose(fp) != 0 || !ok) {
        remove(path);
        return;
    }
    LOG_INFO("No %s here: wrote the default one (change its passwords and name)", path);
}

#ifdef SP_SWITCH_HOST
/* the Switch's local match links the server into the game (the game's own
   main is another): the game starts this on a thread */
#define main sd_server_main
#endif
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_help();
            return 0;
        }
    }
    /* first: the profile picks the config file and the data folder */
    if (server_profile_parse(argc, argv) != 0) {
        return 1;
    }
    const char* data_root = resolve_data_root(argc, argv);
    if (data_root != NULL && chdir(data_root) != 0) {
        fprintf(stderr, "Cannot enter data root '%s'\n", data_root);
        return 1;
    }
    const char* data_folder = resolve_data_dir(argc, argv);
    if (data_dir_set(data_folder) != 0) {
        fprintf(stderr, "Cannot use the data folder '%s'\n", data_folder);
        return 1;
    }
    if (*data_dir() != '\0') {
        LOG_INFO("Data folder: %s", data_dir());
    }
#ifndef _WIN32
    char logs_dir[1100];
    mkdir(data_path(logs_dir, sizeof logs_dir, "logs"), 0755);
#ifdef HAVE_EXECINFO
    data_path(crash_log_path, sizeof crash_log_path, "logs/crash.log");
    signal(SIGSEGV, _crash_handler);
    signal(SIGABRT, _crash_handler);
    signal(SIGBUS, _crash_handler);
#endif
#endif
    /* a missing config is an error, never a fall back to the other
       profile's: the 0.75 server on config.toml would take the 1.0 port
       and fight the other server for it */
    struct stat config_st;
    /* only in a root that was named (--root) or found (it holds the other
       profile's config): a server started from the wrong folder still says
       so below instead of filling that folder */
    if (data_root != NULL && stat(server_config_path(), &config_st) != 0) {
        _write_default_config();
    }
    if (stat(server_config_path(), &config_st) != 0) {
        char cwd[768];
        if (getcwd(cwd, sizeof cwd) == NULL) {
            snprintf(cwd, sizeof cwd, ".");
        }
        fprintf(stderr, "Config file '%s' not found (data root %s)\n", server_config_path(), cwd);
        if (!strcmp(server_config_path(), "Resources/config-075.toml")) {
            fprintf(stderr,
                    "The 0.75 profile reads its own config. Make it from the 1.0 one, then change its port:\n"
                    "  cp Resources/config.toml Resources/config-075.toml\n");
        } else if (!strcmp(server_config_path(), "Resources/config.toml")) {
            fprintf(stderr, "Start the server from its folder, or give it --root <dir> (the folder holding Resources/)\n");
        }
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check-config")) {
            char errors[8192];
            int  n = config_check(server_config_path(), errors, sizeof errors);
            if (errors[0] != '\0') {
                fputs(errors, n > 0 ? stderr : stdout);
            }
            if (n == 0) {
                printf("%s: no errors\n", server_config_path());
            }
            return n == 0 ? 0 : 1;
        }
    }
    uint16_t    port;
    uint8_t     master;
    uint8_t     gamemode;
    uint8_t     aim_assist;
    int         fog_distance;
    uint8_t     capture_limit;
    const char* manager_passwd;
    const char* admin_passwd;
    const char* mod_passwd;
    const char* guard_passwd;
    const char* trusted_passwd;
    const char* server_name;
    const char* team1_name;
    const char* team2_name;
    const char* preset_name = "";
    const char* preset_arg  = NULL;
    color_t     team1_color;
    color_t     team2_color;
    uint8_t     periodic_delays[5];

    string_node_t* map_list              = NULL;
    string_node_t* welcome_message_list  = NULL;
    string_node_t* periodic_message_list = NULL;
    const char*         rotation_mode_str     = NULL;
    map_rotation_mode_t rotation_mode         = MAP_ROTATION_TOML_DEFINED;

    size_t map_list_len;
    size_t welcome_message_list_len;
    size_t periodic_message_list_len;

    toml_table_t* parsed;
    toml_table_t* server_table;
    TOMLH_READ_FROM_FILE(parsed, server_config_path());
    config_keys_log(parsed); /* every key this profile does not read, at every start */
    map_download_configure(parsed);
    sky_configure(parsed); /* [sky]: the 1.0 profile's time of day and weather */
    TOMLH_GET_TABLE(parsed, server_table, "server");
    /* the 0.75 profile's own sections ([ext075], [master075]); the 1.0
       profile ignores them (1.0 has its own ways) */
    if (profile_is_075()) {
        ext075_configure(parsed);
        master075_configure(parsed);
    }

    /* [server] */
    static char default_name[] = "{game}";
    TOMLH_GET_STRING(server_table, server_name, "name", default_name, 1);
    server_name = expand_game_name((char*) server_name, server_name != default_name);
    /* two servers of one data folder told apart on the master's list (tests) */
    const char* name_env = getenv("SP_SERVER_NAME");
    if (name_env != NULL && *name_env != '\0') {
        free((char*) server_name);
        server_name = strdup(name_env);
    }
    TOMLH_GET_INT(server_table, port, "port", DEFAULT_SERVER_PORT, 0);

    /* --port <n> overrides the config value; used by the client listen server
       so it can pick a free port before spawning this process */
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--port")) {
            port = (uint16_t) strtol(argv[i + 1], NULL, 10);
            break;
        }
    }

    TOMLH_GET_BOOL(server_table, master, "master", 1, 0);
    TOMLH_GET_INT(server_table, gamemode, "gamemode", 0, 0);
    TOMLH_GET_INT(server_table, capture_limit, "capture_limit", 10, 0);
    TOMLH_GET_BOOL(server_table, aim_assist, "aim_assist", 1, 1);
    /* the player cap: [server] max_players, at most the profile's. The 0.75
       one is a hard limit, whatever the config says: a 0.75 client cannot
       see a player id of 32 or more, and every 0.75 master expects 32 */
    int profile_cap = profile_is_075() ? PROFILE_075_MAX_PLAYERS : SERVER_MAX_PLAYERS;
    int max_players = profile_cap;
    toml_datum_t max_players_datum = toml_int_in(server_table, "max_players");
    if (max_players_datum.ok) {
        if (max_players_datum.u.i > profile_cap) {
            LOG_WARNING("%s [server] max_players = %lld: the %s profile takes %d at most", server_config_path(),
                        (long long) max_players_datum.u.i, profile_is_075() ? "0.75" : "1.0", profile_cap);
        } else if (max_players_datum.u.i < 1) {
            LOG_WARNING("%s [server] max_players = %lld: at least 1, using %d", server_config_path(),
                        (long long) max_players_datum.u.i, profile_cap);
        } else {
            max_players = (int) max_players_datum.u.i;
        }
    }
    /* tests switch it without editing the config */
    const char* aim_assist_env = getenv("SP_AIM_ASSIST");
    if (aim_assist_env != NULL && *aim_assist_env != 0) {
        aim_assist = atoi(aim_assist_env) != 0;
    }
    get_server()->aim_assist = aim_assist ? 1 : 0;
    LOG_INFO("Gamepad aim assist: %s", aim_assist ? "allowed" : "forbidden");
    TOMLH_GET_INT(server_table, fog_distance, "fog_distance", 128, 1);

    TOMLH_GET_STRING_ARRAY_AS_DL(server_table, map_list, map_list_len, "maps", 0);

    if (map_list_len == 0) {
        LOG_ERROR("No maps defined in %s [server] maps array", server_config_path());
        LOG_ERROR("Please add at least one map to the 'maps' array in your config");
        exit(EXIT_FAILURE);
    }

    // Get map rotation mode
    const char* rotation_mode_default = "toml";
    TOMLH_GET_STRING(server_table, rotation_mode_str, "map_rotation_mode", rotation_mode_default, 0);
    if (rotation_mode_str != NULL) {
        if (strcmp(rotation_mode_str, "alphabetic") == 0) {
            rotation_mode = MAP_ROTATION_ALPHABETIC;
        } else if (strcmp(rotation_mode_str, "random") == 0) {
            rotation_mode = MAP_ROTATION_RANDOM;
        } else if (strcmp(rotation_mode_str, "toml") == 0) {
            rotation_mode = MAP_ROTATION_TOML_DEFINED;
        } else {
            LOG_WARNING("Unknown map_rotation_mode '%s', defaulting to toml", rotation_mode_str);
            rotation_mode = MAP_ROTATION_TOML_DEFINED;
        }
    }

    /* [presets.<name>] or --preset <name>: a mode's rotation and rules in one
       table, so a host does not edit the base server for every mode. The
       command line --gamemode/--map still wins below. */
    TOMLH_GET_STRING(server_table, preset_name, "preset", "", 1);
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--preset")) {
            preset_arg = argv[i + 1];
        }
    }
    if (preset_arg != NULL) {
        preset_name = preset_arg;
    }
    const char* preset_env = getenv("SP_PRESET");
    if (preset_env != NULL && *preset_env != '\0') {
        preset_name = preset_env;
    }
    if (preset_name != NULL && *preset_name != '\0') {
        toml_table_t* presets = toml_table_in(parsed, "presets");
        toml_table_t* p       = presets != NULL ? toml_table_in(presets, preset_name) : NULL;
        if (p == NULL) {
            LOG_ERROR("Preset '%s' is not in %s [presets.%s]", preset_name, server_config_path(), preset_name);
            exit(EXIT_FAILURE);
        }
        TOMLH_GET_INT(p, gamemode, "gamemode", gamemode, 1);
        TOMLH_GET_INT(p, capture_limit, "capture_limit", capture_limit, 1);
        TOMLH_GET_BOOL(p, aim_assist, "aim_assist", aim_assist, 1);
        TOMLH_GET_INT(p, fog_distance, "fog_distance", fog_distance, 1);
        maps_from_table(p, "maps", &map_list, &map_list_len);
        get_server()->aim_assist = aim_assist ? 1 : 0;
        LOG_INFO("Preset: %s", preset_name);
        if (map_list_len == 0) {
            LOG_ERROR("Preset '%s' has no maps", preset_name);
            exit(EXIT_FAILURE);
        }
    }

    /* the fog every 1.0 client draws (GameStateFinal's fog-distance) and the
       distance past which enemies are no longer sent (Bridge.c), bots'
       sight too: 128 is 0.75's; tests set it with SP_FOG_DISTANCE */
    const char* fog_env = getenv("SP_FOG_DISTANCE");
    if (fog_env != NULL && *fog_env != 0) {
        fog_distance = atoi(fog_env);
    }
    if (fog_distance < 32 || fog_distance > 256) {
        LOG_WARNING("fog_distance %d is outside 32..256, using 128", fog_distance);
        fog_distance = 128;
    }
    /* a 0.75 client draws its fog at 128 whatever we say (0.75 has no fog
       distance on the wire), so the 0.75 profile keeps the bots' sight and
       every distance band at 128: a longer one let bots shoot from inside
       a fog the 0.75 player cannot see through */
    if (profile_is_075() && fog_distance != 128) {
        LOG_WARNING("fog_distance %d ignored: the 0.75 profile's fog is 128, a 0.75 client's", fog_distance);
        fog_distance = 128;
    }
    get_server()->fog_distance = (uint16_t) fog_distance;
    LOG_INFO("Fog distance: %d blocks", fog_distance);

    /* --gamemode <name> and --map <Name>: what the player chose in the
       client's picker (common/modes/modes.h, the names are the server's
       game mode names). A chosen map replaces the
       rotation, so a won match plays it again instead of another mode's map.
       --maps-dir <dir>: a folder searched before Resources/maps (the
       client's player maps, absolute: we chdir'ed to the root), read
       first so --map finds a map in it */
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--maps-dir")) {
            map_set_user_dir(argv[i + 1]);
            LOG_STATUS("Player maps: %s", argv[i + 1]);
        }
    }
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--gamemode")) {
            /* asked before gamemode_find, which knows the 1.0 modes too: a
               1.0 mode named to the 0.75 server is not "unknown" */
            if (profile_is_075() && !profile_075_mode(argv[i + 1])) {
                LOG_ERROR("Game mode '%s' is not in the 0.75 profile (it has ctf, tc and babel)", argv[i + 1]);
                exit(EXIT_FAILURE);
            }
            const gamemode_ops_t* mode = gamemode_find(argv[i + 1]);
            if (mode == NULL) {
                LOG_ERROR("Unknown game mode '%s' (ctf, babel, arena, zombie, or a plugin Resources/plugins/<name>.lua)",
                          argv[i + 1]);
                exit(EXIT_FAILURE);
            }
            gamemode = (uint8_t) mode->id;
        } else if (!strcmp(argv[i], "--classes")) {
            /* the host's choice for the classes (the zombie mode's): 1 the
               survivors pick a class, 0 they do not */
            get_server()->classes_flag = atoi(argv[i + 1]) != 0 ? 1 : 2;
        } else if (!strcmp(argv[i], "--first-infected")) {
            /* the host's choice (the setup screen's "First infected"): over
               config.toml's [zombie] first_infected */
            int n = !strcmp(argv[i + 1], "auto") ? 0 : atoi(argv[i + 1]);
            n     = n < 0 ? 0 : n > 64 ? 64 : n;
            get_server()->first_infected_flag = n == 0 ? 255 : (uint8_t) n;
        } else if (!strcmp(argv[i], "--map")) {
            const char* name = argv[i + 1];
            char        path[512];
            char        folder[MAP_NAME_MAX + 1];
            struct stat st;
            /* a name, not a path: it is joined into one */
            if (strchr(name, '/') == NULL && strchr(name, '\\') == NULL && map_name_ok(name)) {
                if (map_resolve_alias(name, folder, sizeof folder, 1)) {
                    name = folder;
                }
                map_download_now(name);
            }
            map_file_path(path, sizeof path, name, "vxl");
            if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || stat(path, &st) != 0) {
                LOG_ERROR("Map '%s' not found (%s)", name, path);
                exit(EXIT_FAILURE);
            }
            string_node_t *node, *tmp;
            DL_FOREACH_SAFE(map_list, node, tmp)
            {
                DL_DELETE(map_list, node);
                free(node->string);
                free(node);
            }
            node         = spadesx_malloc(sizeof(string_node_t));
            node->string = strdup(name);
            DL_APPEND(map_list, node);
            map_list_len = 1;
        }
    }
    /* a configured (or preset) mode the 0.75 profile does not play becomes
       classic CTF, as gamemode_init does: said here, so the "Mode:" line and
       the map rule below are the ones that will play */
    if (profile_is_075()) {
        const gamemode_ops_t* configured = gamemode_ops_of(gamemode);
        if (configured == NULL || !profile_075_mode(configured->name)) {
            const gamemode_ops_t* ctf = gamemode_find("ctf");
            LOG_WARNING("Game mode '%s' of %s is not in the 0.75 profile (it has ctf, tc and babel): playing ctf",
                        configured != NULL ? configured->name : "unknown", server_config_path());
            if (ctf != NULL) {
                gamemode = (uint8_t) ctf->id;
            }
        }
    }
    /* before map_fits, which reads a map's toml */
    install_rotation(&map_list, &map_list_len);
    if (map_list == NULL) {
        LOG_ERROR("None of the maps of %s [server] maps is installed%s", server_config_path(),
                  map_download_enabled() ? ", and they could not be downloaded" : "");
        LOG_ERROR("Copy their folders from https://github.com/ngspades/maps (maps/<Name>/) into "
                  "Resources/maps/, or start with --maps-dir <folder>%s",
                  map_download_enabled() ? "" : ", or set [maps] download = true");
        exit(EXIT_FAILURE);
    }
    /* the mode's own map rule (map_fits): a rotation written for another mode
       loses the maps that do not suit this one, and with none left the
       mode's catalog maps play */
    const gamemode_ops_t* chosen = gamemode_ops_of(gamemode);
    if (chosen != NULL && chosen->map_fits != NULL) {
        string_node_t *node, *tmp;
        DL_FOREACH_SAFE(map_list, node, tmp)
        {
            if (!chosen->map_fits(node->string)) {
                LOG_WARNING("Map '%s' does not suit the %s mode, skipped", node->string, chosen->name);
                DL_DELETE(map_list, node);
                free(node->string);
                free(node);
                map_list_len--;
            }
        }
        const struct game_mode_info* info = game_mode_find(chosen->name);
        if (map_list == NULL && info != NULL) {
            for (int j = 0; j < game_mode_map_count(info); j++) {
                node         = spadesx_malloc(sizeof(string_node_t));
                node->string = strdup(info->maps[j]);
                DL_APPEND(map_list, node);
                map_list_len++;
            }
            install_rotation(&map_list, &map_list_len);
        }
        if (map_list == NULL) {
            LOG_ERROR("No map suits the %s mode", chosen->name);
            exit(EXIT_FAILURE);
        }
    }
    LOG_INFO("Mode: %s, map: %s",
             gamemode_ops_of(gamemode) != NULL ? gamemode_ops_of(gamemode)->name : "unknown",
             map_list->string);

    TOMLH_GET_INT_ARRAY(server_table, periodic_delays, "periodic_delays", 5, ((uint8_t[]){1, 5, 10, 30, 60}), 1);
    TOMLH_GET_STRING_ARRAY_AS_DL(server_table, welcome_message_list, welcome_message_list_len, "welcome_messages", 1);
    TOMLH_GET_STRING_ARRAY_AS_DL(server_table, periodic_message_list, periodic_message_list_len, "periodic_messages", 1);
    string_node_t* message;
    DL_FOREACH(welcome_message_list, message)
    {
        message->string = expand_game_name(message->string, 1);
    }
    DL_FOREACH(periodic_message_list, message)
    {
        message->string = expand_game_name(message->string, 1);
    }

    /* [teams] */
    toml_table_t* teams_table;
    toml_table_t* team1_table;
    toml_table_t* team2_table;
    TOMLH_GET_TABLE(parsed, teams_table, "teams");

    TOMLH_GET_TABLE(teams_table, team1_table, "team1");
    TOMLH_GET_STRING(team1_table, team1_name, "name", "Blue Team", 0);
    TOMLH_GET_RGB_COLOR(team1_table, team1_color, "color", ((uint8_t[]){0, 0, 255}), 0);

    TOMLH_GET_TABLE(teams_table, team2_table, "team2");
    TOMLH_GET_STRING(team2_table, team2_name, "name", "Green Team", 0);
    TOMLH_GET_RGB_COLOR(team2_table, team2_color, "color", ((uint8_t[]){0, 255, 0}), 0);

    /* [passwords] */
    toml_table_t* passwords_table;
    TOMLH_GET_TABLE(parsed, passwords_table, "passwords");
    TOMLH_GET_STRING(passwords_table, manager_passwd, "manager", "", 0);
    TOMLH_GET_STRING(passwords_table, admin_passwd, "admin", "", 0);
    TOMLH_GET_STRING(passwords_table, mod_passwd, "moderator", "", 0);
    TOMLH_GET_STRING(passwords_table, guard_passwd, "guard", "", 0);
    TOMLH_GET_STRING(passwords_table, trusted_passwd, "trusted", "", 0);
    warn_default_passwords((const char* const[]){manager_passwd, admin_passwd, mod_passwd, guard_passwd,
                                                 trusted_passwd});

    server_args args = {.port                      = port,
                        /* the ENet host's connections and the server's
                           max_players (Server.c caps both with it) */
                        .connections               = (uint32_t) max_players,
                        .channels                  = 2,
                        .in_bandwidth              = 0,
                        .out_bandwidth             = 0,
                        .master                    = master,
                        .map_list                  = map_list,
                        .map_count                 = map_list_len,
                        .welcome_message_list      = welcome_message_list,
                        .welcome_message_list_len  = welcome_message_list_len,
                        .periodic_message_list     = periodic_message_list,
                        .periodic_message_list_len = periodic_message_list_len,
                        .periodic_delays           = periodic_delays,
                        .manager_password          = manager_passwd,
                        .admin_password            = admin_passwd,
                        .mod_password              = mod_passwd,
                        .guard_password            = guard_passwd,
                        .trusted_password          = trusted_passwd,
                        .server_name               = server_name,
                        .team1_name                = team1_name,
                        .team2_name                = team2_name,
                        .team1_color               = team1_color,
                        .team2_color               = team2_color,
                        .gamemode                  = gamemode,
                        .capture_limit             = capture_limit,
                        .map_rotation_mode         = rotation_mode,
                        .preset_name               = preset_name};

    /* The server is started by our own client (listen server / --server) or
       launched explicitly with --listen; the local player then owns it. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen") || !strcmp(argv[i], "--server")) {
            get_server()->listen_mode = 1;
        }
    }
    /* a match the player started from the menu (--listen) is private
       unless the host ticked "List on the server list" (--public); the
       dedicated server the client starts (--server) is listed as before */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen")) {
            get_server()->listen_private = 1;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--public")) {
            get_server()->listen_private = 0;
        }
    }
    /* the match's handy join code (friends join it with WebRTC, no port
       forwarding): the client that starts the listen server picks it, or
       SP_JOIN_CODE for a test. A plain address still works when there
       is one */
    const char* join_code_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--join-code") && i + 1 < argc) {
            join_code_arg = argv[i + 1];
        }
    }
    if (join_code_arg == NULL) {
        const char* env_code = getenv("SP_JOIN_CODE");
        if (env_code != NULL && *env_code != '\0') {
            join_code_arg = env_code;
        }
    }
    if (join_code_arg != NULL) {
        /* the player's paste: separators and the case out */
        char   out[sizeof(get_server()->join_code)];
        size_t n = 0;
        for (const char* c = join_code_arg; *c && n + 1 < sizeof(out); c++) {
            if (*c == '-' || *c == ' ') {
                continue;
            }
            out[n++] = (char) (*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
        }
        out[n] = '\0';
        if (n == 6) {
            snprintf(get_server()->join_code, sizeof(get_server()->join_code), "%s", out);
            /* the host sees in the log what its friends will type (and a
               test reads it without the master) */
            LOG_STATUS("Join code: %c%c%c-%c%c%c", out[0], out[1], out[2], out[3], out[4], out[5]);
        } else {
            LOG_WARNING("--join-code %s: a code is six letters and digits (like ABC-DEF)", join_code_arg);
        }
    }

    int startup_bots = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bots") && i + 1 < argc) {
            startup_bots = atoi(argv[i + 1]);
        }
        if (!strcmp(argv[i], "--bots-test")) {
            startup_bots = -1; /* exactly one enemy bot, for testing combat */
        }
        /* singleplayer's setup screen: so many bots on each team */
        if (!strcmp(argv[i], "--team-bots") && i + 2 < argc) {
            bots_set_startup_teams(atoi(argv[i + 1]), atoi(argv[i + 2]));
        }
    }

    /* the banner: which server this is, one fact a line, before the first
       map loads (docs/plan-075-server.md 4.4; /profile prints the same) */
    char banner[PROFILE_REPORT_MAX][PROFILE_REPORT_LINE];
    int  banner_lines = profile_report_lines(port, max_players, banner, PROFILE_REPORT_MAX);
    for (int i = 0; i < banner_lines; i++) {
        LOG_INFO("%s", banner[i]);
    }

    server_start(args, startup_bots);

    free((char*) server_name);
    free((char*) manager_passwd);
    free((char*) admin_passwd);
    free((char*) mod_passwd);
    free((char*) guard_passwd);
    free((char*) trusted_passwd);
    free((char*) team1_name);
    free((char*) team2_name);
    if (rotation_mode_str != rotation_mode_default) {
        free((char*) rotation_mode_str);
    }
    toml_free(parsed);

    return 0;
}
