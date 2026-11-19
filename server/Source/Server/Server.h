// Copyright DarkNeutrino 2021
#ifndef SERVER_H
#define SERVER_H

#define VERSION \
    "Beta 2.0"  \
    " Compiled on " __DATE__ " " __TIME__

#include <Server/Structs/ServerStruct.h>
#include <Server/Structs/StartStruct.h>
#include <Util/Enums.h>
#include <Util/Line.h>
#include <Util/Queue.h>
#include <Util/Types.h>
#include <enet/enet.h>
#include <Server/MapStore.h>
#include <pthread.h>

/* players in a match, 64 a team (protocol 1.0; 0.75 allowed 32) */
#define SERVER_MAX_PLAYERS 128

#ifndef DEFAULT_SERVER_PORT
    #define DEFAULT_SERVER_PORT 32887
#endif

extern pthread_mutex_t server_lock;

server_t* get_server(void);

void stop_server(void);

void server_start(server_args args, int startup_bots);

void server_reset(server_t* server);

/* /map and /restart: the next load uses this map, whether or not it is in the
   rotation. 0 when the name is unsafe or the files are missing. */
int server_force_map(server_t* server, const char* name);

#endif /* SERVER_H */
