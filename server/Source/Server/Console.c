#include <Server/Commands/Commands.h>
#include <Server/Server.h>
#include <Server/Structs/ServerStruct.h>
#include <Util/Log.h>
#include <pthread.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <time.h>

/* nanosleep exists everywhere we build (POSIX, and MinGW's winpthreads);
   sleep() is not reliably declared on Windows */
static void _console_sleep(time_t seconds)
{
    struct timespec t = {seconds, 0};
    nanosleep(&t, NULL);
}

#ifdef __APPLE__
/* macOS ships libedit as "readline"; the GNU helpers below do not exist
   there, so provide local no-ops to keep the console working */
static void rl_replace_line(const char* text, int clear_undo)
{
    (void) text;
    (void) clear_undo;
}
static void rl_clear_history(void) {}
#endif

volatile int ctrlc = 0;

void handle_sigint(void)
{
    ctrlc = 1;
    printf("\n");
    LOG_INFO_WITHOUT_TIME("Are you sure you want to exit? (Y/n)\n");
    rl_replace_line("", 0);
    rl_on_new_line();
    rl_redisplay();
}

void* server_console(void* arg)
{
    server_t* server = (server_t*) arg;
    char*     buf;

    /* Detached/background runs have no terminal (for example when the server
       is spawned by the game for a listen server). Readline would return EOF
       immediately and shut the server down, so only run the interactive
       console when stdin is actually usable. */
    if (!server->console_enabled) {
        while (server->running) {
            _console_sleep(1);
        }
        return 0;
    }

    while ((buf = readline("\x1B[0;34mConsole> \x1B[0;37m")) != NULL) {
        if (ctrlc) { // "Are you sure?" prompt
            if (strcmp("n", buf) != 0) {
                break;
            }
            ctrlc = 0;
        } else if (strlen(buf) > 0) {
            add_history(buf);
            pthread_mutex_lock(&server_lock);
            player_t* player = NULL;
            command_handle(server, player, buf, 1);
            pthread_mutex_unlock(&server_lock);
        }
        free(buf);
    }
    rl_clear_history();
    stop_server();

    _console_sleep(5); // wait 5 seconds for the server to stop

    // if this thread is not dead at this point, then we need to stop the server by force >:)
    LOG_ERROR("Server did not respond for 5 seconds. Killing it with fire...");
    exit(-1);

    return 0;
}
