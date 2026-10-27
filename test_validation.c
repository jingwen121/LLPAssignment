// Integration tests use real DDS participants in separate processes
// The small server below is a protocol fixture; it does not implement game mechanics
// Run without another game using DDS domain 42 so discovery cannot mix the runs
#include "network.h"
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// Exit normally on failure so the parent can stop and reap its test processes
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    exit(EXIT_FAILURE); } } while (0)

// The parent chooses a scenario; the child sends it through its own DDS writer
// Attack commands bypass net_client_send so local checks cannot hide server bugs
enum { SEND_INPUT = 1, REPLAY, RETRY_JOIN, FORGE_JOIN, FLOOD, FORGE_STATE,
       SAVE_SESSION, OLD_SESSION, CLOSE };

// Share test commands and counters between processes, never DDS handles
static struct Shared
{
    // Count events after the server validates them; a successful DDS write alone
    // does not prove the server accepted movement or allocated a player slot
    atomic_int server_ready;
    atomic_int joins;
    atomic_int leaves;
    atomic_int moves;
    atomic_ullong rejected;
    atomic_llong received_at;
    atomic_int bad_snapshot;
    atomic_bool stop_server;
    struct Client
    {
        // start controls admission order; pause suppresses sends without closing
        // DDS, which lets us test heartbeat expiry while the process stays alive
        atomic_bool start;
        atomic_bool pause;
        atomic_bool stop;
        atomic_int ready;
        atomic_int player_id;
        atomic_int updates;
        atomic_int payload_checks;
        atomic_ullong session_id;
        atomic_ullong rejected;

        // The parent fills the command arguments before publishing command
        // The child sets done after issuing the operation, not after server receipt
        atomic_int command;
        atomic_int done;
        atomic_llong sent_at;
        int input_id;
        char direction;
        uint8_t guid[16];
        uint8_t forged_guid[16];
    } clients[6];
} *shared;

// Index 0 is the server; indexes 1 to 6 are the 6 independent client processes
// Clear an entry after waitpid so cleanup cannot signal a PID that has been reused
static pid_t children[7];
static pid_t parent_pid;

static void cleanup(void)
{
    // Children inherit atexit registrations; only the original parent owns the
    // process list; A failing child must not accidentally terminate its siblings
    if (getpid() != parent_pid)
        return;

    // Stop and reap child processes even if an assertion ends the test early
    for (int i = 0; i < 7; ++i)
    {
        if (children[i] > 0)
        {
            kill(children[i], SIGTERM);
            waitpid(children[i], NULL, 0);
        }
    }
}

