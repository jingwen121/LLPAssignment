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

static void test_server(const char *root)
{
    // Use the same secure network implementation as the game, with predictable
    // player data so failures are not affected by random spawns or coin catches
    Network net;
    char directory[4096];
    snprintf(directory, sizeof(directory), "%s/server", root);
    if (!net_open(&net, true, directory))
    {
        atomic_store(&shared->server_ready, -1);
        exit(1);
    }

    // Populate every part of the agreed payload without simulating a round
    // This checks transport of game-state fields, not coin spawning or winner logic
    GameState state = {.state = 1, .remaining_ms = 60000};
    for (int i = 0; i < NET_COINS; ++i)
        state.coins[i] = (CoinState){true, i * 160, -i * 100, i == 0};
    int64_t last_snapshot = 0;
    atomic_store(&shared->server_ready, 1);

    while (!atomic_load(&shared->stop_server))
    {
        // Cap event work per pass so incoming traffic cannot postpone every
        // snapshot; NET_NONE can also mean that a heartbeat was consumed
        for (int count = 0; count < 64; ++count)
        {
            NetEvent event = net_server_poll(&net);
            if (event.type == NET_NONE)
                break;

            if (event.type == NET_JOIN)
            {
                int id = 0;

                // Give the next free slot to this client
                for (int i = 0; i < NET_PLAYERS; ++i)
                {
                    if (!state.players[i].active)
                    {
                        id = i + 1;
                        state.players[i] = (PlayerState){true, 100 + i * 100, 500, 3};
                        break;
                    }
                }
                CHECK(net_server_accept(&net, id));

                // ID 0 replies that capacity is exhausted; do not count that as
                // an admitted player when checking duplicate joins and slot reuse
                if (id > 0)
                    atomic_fetch_add(&shared->joins, 1);
            }
            else if (event.type == NET_LEAVE)
            {
                state.players[event.player_id - 1] = (PlayerState){0};
                atomic_fetch_add(&shared->leaves, 1);
            }
            else if (event.type == NET_MOVE)
            {
                // We only count the validated event here; simulating movement
                // would mix game-rule failures into the networking tests
                atomic_store(&shared->received_at, net_now());
                atomic_fetch_add(&shared->moves, 1);
            }
        }
        atomic_store(&shared->rejected, net.rejected);

        // Exercise each bounds check independently; the parent waits for this
        // sample to be rejected before requesting another one; depth 1 must not
        // turn the test into a race between several malformed snapshots
        int bad_kind = atomic_exchange(&shared->bad_snapshot, 0);
        if (bad_kind)
        {
            GameState bad = state;
            switch (bad_kind)
            {
                case 1: bad.players[0].x = INT_MAX; break;
                case 2: bad.players[0].x = -1; break;
                case 3: bad.players[0].y = 551; break;
                case 4: bad.players[0].y = -1; break;
                case 5: bad.players[0].score = -1; break;
                case 6: bad.state = -1; break;
                case 7: bad.state = 3; break;
                case 8: bad.remaining_ms = -1; break;
                case 9: bad.remaining_ms = 60001; break;
                case 10: bad.winner_id = -1; break;
                case 11: bad.winner_id = 6; break;
                case 12: bad.coins[0].x = -1; break;
                case 13: bad.coins[0].x = 801; break;
                case 14: bad.coins[0].y = -601; break;
                case 15: bad.coins[0].y = 601; break;
            }

            // Kind 16 deliberately bypasses net_server_state so the sequence is
            // repeated; every other case gets a new sequence with bad content
            if (bad_kind == 16)
                CHECK(dds_write(net.state, &state) == DDS_RETCODE_OK);
            else
                CHECK(net_server_state(&net, &bad));
            last_snapshot = net_now();
        }
        else if (net_now() - last_snapshot >= INT64_C(50000000))
        {
            // Match the game's 20 Hz snapshot schedule; each sample contains
            // enough state to recover without receiving earlier snapshots
            CHECK(net_server_state(&net, &state));
            last_snapshot = net_now();
        }
        dds_sleepfor(DDS_MSECS(1));
    }
    net_close(&net);
    exit(0);
}

static void test_client(const char *root, int index)
{
    struct Client *report = &shared->clients[index];
    while (!atomic_load(&report->start))
        usleep(1000);

    char directory[4096];
    snprintf(directory, sizeof(directory), "%s/client%d", root, index + 1);
    Network net;
    if (!net_open(&net, false, directory))
    {
        atomic_store(&report->ready, -1);
        exit(1);
    }

    // Publish the GUID before ready; the parent needs a real second identity
    // when constructing an impersonation attempt from the first client
    memcpy(report->guid, net.own_id.v, sizeof(report->guid));
    atomic_store(&report->ready, 1);
    uint64_t previous = 0;
    uint64_t old_session = 0;

    while (!atomic_load(&report->stop))
    {
        GameState snapshot;
        if (net_client_state(&net, &snapshot))
        {
            atomic_fetch_add(&report->updates, 1);

            // Verify the values, not just arrival counts; A schema mismatch can
            // deliver a sample while making coins or HUD fields unusable for the client
            CHECK(snapshot.state == 1 && snapshot.remaining_ms == 60000);
            for (int i = 0; i < NET_COINS; ++i)
                CHECK(snapshot.coins[i].active && snapshot.coins[i].x == i * 160 &&
                      snapshot.coins[i].y == -i * 100 && snapshot.coins[i].gold == (i == 0));
            for (int i = 0; i < NET_PLAYERS; ++i)
                if (snapshot.players[i].active)
                    CHECK(snapshot.players[i].score == 3);
            atomic_fetch_add(&report->payload_checks, 1);
        }

        int command = atomic_load(&report->command);
        if (command == SEND_INPUT || command == REPLAY || command == FLOOD ||
            command == OLD_SESSION)
        {
            // REPLAY repeats a previous sequence; OLD_SESSION advances the
            // sequence but supplies an expired token so we isolate that check
            // FLOOD removes the normal send interval and attempts 2000 writes
            int count = command == FLOOD ? 2000 : 1;
            for (int i = 0; i < count; ++i)
            {
                Input input = {report->input_id, report->direction,
                               command == REPLAY ? previous : ++net.seq,
                               command == OLD_SESSION ? old_session : net.session_id};
                if (command != REPLAY)
                    previous = input.seq;

                // Record send time immediately before writing to DDS
                atomic_store(&report->sent_at, net_now());
                CHECK(dds_write(net.input, &input) == DDS_RETCODE_OK);
            }
        }
        else if (command == RETRY_JOIN || command == FORGE_JOIN)
        {
            // A retry uses our real GUID; a forgery claims another participant's
            // GUID while DDS metadata still identifies this sending process
            JoinRequest request;
            memcpy(request.client_id, command == RETRY_JOIN ? report->guid :
                   report->forged_guid, sizeof(request.client_id));
            CHECK(dds_write(net.join, &request) == DDS_RETCODE_OK);
        }
        else if (command == FORGE_STATE)
        {
            // Both server-owned topics must reject client writers; this checks
            // DDS permissions, not a voluntary restriction in our client API
            dds_entity_t topic = dds_create_topic(net.participant, &GameState_desc,
                                                  "GameState", NULL, NULL);
            CHECK(topic > 0);
            CHECK(dds_create_writer(net.participant, topic, NULL, NULL) < 0);
            topic = dds_create_topic(net.participant, &JoinResponse_desc,
                                    "JoinResponse", NULL, NULL);
            CHECK(topic > 0);
            CHECK(dds_create_writer(net.participant, topic, NULL, NULL) < 0);

            // Clients also have no reason to read another player's raw inputs
            topic = dds_create_topic(net.participant, &Input_desc, "Input", NULL, NULL);
            CHECK(topic > 0);
            CHECK(dds_create_reader(net.participant, topic, NULL, NULL) < 0);
        }
        else if (command == SAVE_SESSION)
            old_session = net.session_id;
        else if (command == CLOSE)
        {
            net_close(&net);
            atomic_store(&report->done, 1);
            exit(0);
        }

        if (command != 0)
        {
            // Clear the request before signalling completion; otherwise the
            // next parent command could be erased by this child's old request
            atomic_store(&report->command, 0);
            atomic_store(&report->done, 1);
        }

        if (!atomic_load(&report->pause))
            CHECK(net_client_send(&net, 'n'));

        // Report the network layer's current assignment, including -1 after
        // timeout; do not infer connection state from the last drawn snapshot
        atomic_store(&report->player_id, net.player_id);
        atomic_store(&report->session_id, net.session_id);
        atomic_store(&report->rejected, net.rejected);
        dds_sleepfor(DDS_MSECS(1));
    }
    net_close(&net);
    exit(0);
}

static void wait_ms(int milliseconds)
{
    usleep((useconds_t)milliseconds * 1000);
}

static void command(int client, int operation, int id, char direction)
{
    struct Client *report = &shared->clients[client];

    // The atomic command store publishes these arguments to the child; only
    // 1 request is outstanding per client, so no command queue is needed
    report->input_id = id;
    report->direction = direction;
    atomic_store(&report->done, 0);
    atomic_store(&report->command, operation);

    // A failed child must produce a failed test instead of an endless wait
    int64_t deadline = net_now() + INT64_C(2000000000);
    while (!atomic_load(&report->done) && net_now() < deadline)
        wait_ms(1);
    CHECK(atomic_load(&report->done));
}

static int player_id(int client)
{
    return atomic_load(&shared->clients[client].player_id);
}

static int compare_time(const void *a, const void *b)
{
    // Compare without subtracting timestamps; qsort needs an int result and
    // narrowing a large nanosecond difference could reverse the ordering
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

