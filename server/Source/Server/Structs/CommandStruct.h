#ifndef COMMANDSTRUCT_H
#define COMMANDSTRUCT_H

#include <Util/Types.h>
#include <uthash/uthash.h>

typedef struct player player_t;
typedef struct server server_t;

/* A role /login accepts: its name, the passwords it takes (access_password[0]
   is the configured one, and an empty one keeps the role shut) and the
   position of the permission bit it sets in a player's permissions */
typedef struct permissions
{
    char         access_level[32];
    const char** access_password;
    uint8_t      perm_level_offset;
} permissions_t;

/* What a command is run with: argv[0] is the command word itself, the words
   after it are argv[1] and on. The dispatcher owns those and frees them once
   the command returns, so nothing may keep a pointer past the call */
typedef struct command_args
{
    player_t* player;
    server_t* server;
    uint8_t   console;
    uint32_t  permissions;
    uint32_t  argc;
    char*     argv[32]; // 32 roles commands should be more than enough for anyone
} command_args_t;

/* A registered command: its name (with the "/"), whether its arguments are
   split, what it runs and who may run it, and the links of the lookup map
   and of the list /help walks */
typedef struct command
{
    char    id[30];
    uint8_t parse_args; // Should we even bother parsing it? Would be useful for commands whose one and only
                        // argument is a message, or which don't even have any arguments
    void (*execute)(void* p_server, command_args_t arguments);
    uint32_t        permissions; // 32 roles should be more then enough for anyone
    char            description[1024];
    UT_hash_handle  hh;
    struct command* next;
} command_t;

/* A command before it is registered (command_populate_all's table): what
   command_create takes, without the map's and the list's links, so the whole
   registry can be one plain array */
typedef struct command_manager
{
    char    id[30];
    uint8_t parse_args; // Should we even bother parsing it? Would be useful for commands whose one and only
                        // argument is a message, or which don't even have any arguments
    void (*command)(void* p_server, command_args_t arguments);
    uint32_t permissions; // 32 roles should be more then enough for anyone
    char     description[1024];
} command_manager_t;

#endif
