// Integration tests use real DDS participants in separate processes
// The small server below is a protocol fixture; it does not implement game mechanics
// Run without another game using DDS domain 42 so discovery cannot mix the runs
#include "network.h"
#include <limits.h>
#include <pthread.h>
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

        // Real-room tests reuse these child processes with a snapshot copied
        // under a process-shared mutex; DDS handles never cross the boundary
        int room_code;
        bool create_room;
        int attack_room;
        uint64_t attack_session;
        pthread_mutex_t snapshot_mutex;
        GameState snapshot;
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
    GameState state = {.phase = 1, .time_left = 60};
    for (int i = 0; i < NET_COINS; ++i)
    {
        state.coin_active[i] = true;
        state.coin_x[i] = i * (800 / NET_COINS);
        state.coin_y[i] = -i * 60;
        state.coin_gold[i] = i == 0;
        state.coin_size[i] = i == 0 ? 14 : 20;
    }
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
                    if (!state.player_active[i])
                    {
                        id = i + 1;
                        state.player_active[i] = true;
                        state.player_x[i] = 100 + i * 100;
                        state.player_y[i] = 500;
                        state.player_score[i] = 3;
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
                state.player_active[event.player_id - 1] = false;
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
                case 1: bad.player_x[0] = INT_MAX; break;
                case 2: bad.player_x[0] = -1; break;
                case 3: bad.player_y[0] = 551; break;
                case 4: bad.player_y[0] = -1; break;
                case 5: bad.player_score[0] = -1; break;
                case 6: bad.phase = -1; break;
                case 7: bad.phase = 3; break;
                case 8: bad.time_left = -1; break;
                case 9: bad.time_left = 61; break;
                case 10: bad.winners = -1; break;
                case 11: bad.winners = 32; break;
                case 12: bad.coin_x[0] = -1; break;
                case 13: bad.coin_x[0] = 801; break;
                case 14: bad.coin_y[0] = -621; break;
                case 15: bad.coin_y[0] = 621; break;
                case 17: bad.coin_size[0] = INT_MAX; break;
                case 18: bad.best_score = -1; break;
                case 19: bad.room_code = 345678; break;
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

    if (report->room_code)
        net_client_room(&net, report->room_code, report->create_room, (uint64_t)net_now());

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

            if (report->room_code)
            {
                CHECK(snapshot.room_code == report->room_code);
                pthread_mutex_lock(&report->snapshot_mutex);
                report->snapshot = snapshot;
                pthread_mutex_unlock(&report->snapshot_mutex);
            }
            else
            {
                // Verify the values, not just arrival counts; A schema mismatch can
                // deliver a sample while making coins or HUD fields unusable for the client
                CHECK(snapshot.phase == 1 && snapshot.time_left == 60);
                for (int i = 0; i < NET_COINS; ++i)
                    CHECK(snapshot.coin_active[i] && snapshot.coin_x[i] == i * (800 / NET_COINS) &&
                          snapshot.coin_y[i] == -i * 60 && snapshot.coin_gold[i] == (i == 0));
                for (int i = 0; i < NET_PLAYERS; ++i)
                    if (snapshot.player_active[i])
                        CHECK(snapshot.player_score[i] == 3);
                atomic_fetch_add(&report->payload_checks, 1);
            }
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
                Input input = {.room_code = report->attack_room ? report->attack_room : net.room_code,
                    .int_player_id = report->input_id, .ch_direction = report->direction,
                    .leave_room = report->direction == 'q',
                    .seq = report->attack_session ? UINT64_C(1000000) :
                           command == REPLAY ? previous : ++net.seq,
                    .session_id = report->attack_session ? report->attack_session :
                                  command == OLD_SESSION ? old_session : net.session_id};
                if (command != REPLAY)
                    previous = input.seq;

                // Record send time immediately before writing to DDS
                atomic_store(&report->sent_at, net_now());
                CHECK(dds_write(net.input, &input) == DDS_RETCODE_OK);
                if (report->room_code && !report->attack_session)
                {
                    // Keep a scripted key press alive until its next heartbeat
                    net.last_input = net_now();
                }
            }
        }
        else if (command == RETRY_JOIN || command == FORGE_JOIN)
        {
            // A retry uses our real GUID; a forgery claims another participant's
            // GUID while DDS metadata still identifies this sending process
            JoinRequest request = {0};
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

// Reuse the protocol harness's process ownership and cleanup for real rooms
static void start_room_client(const char *root, int i, int code, bool create)
{
    struct Client *report = &shared->clients[i];
    report->room_code = code;
    report->create_room = create;
    report->attack_room = 0;
    report->attack_session = 0;
    atomic_store(&report->stop, false);
    atomic_store(&report->ready, 0);
    atomic_store(&report->player_id, -1);
    atomic_store(&report->updates, 0);
    atomic_store(&report->command, 0);
    atomic_store(&report->start, true);
    fflush(NULL);
    children[i + 1] = fork();
    CHECK(children[i + 1] >= 0);
    if (children[i + 1] == 0)
        test_client(root, i);
}

static void stop_room_client(int i, bool crash)
{
    if (crash)
        CHECK(kill(children[i + 1], SIGKILL) == 0);
    else
        command(i, CLOSE, 0, 0);
    int status;
    CHECK(waitpid(children[i + 1], &status, 0) == children[i + 1]);
    children[i + 1] = 0;
    CHECK(crash ? WIFSIGNALED(status) : WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void room_snapshots(GameState states[3], int milliseconds)
{
    wait_ms(milliseconds);
    for (int i = 0; i < 3; ++i)
    {
        pthread_mutex_lock(&shared->clients[i].snapshot_mutex);
        states[i] = shared->clients[i].snapshot;
        pthread_mutex_unlock(&shared->clients[i].snapshot_mutex);
    }
}

static int test_rooms(const char *root)
{
    // Each certificate gets its own process, matching how the game is launched
    // This also keeps Cyclone's process-local security state independent
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    memset(shared, 0, sizeof(*shared));
    parent_pid = getpid();
    atexit(cleanup);
    pthread_mutexattr_t attr;
    CHECK(pthread_mutexattr_init(&attr) == 0);
    CHECK(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0);
    for (int i = 0; i < 3; ++i)
        CHECK(pthread_mutex_init(&shared->clients[i].snapshot_mutex, &attr) == 0);
    pthread_mutexattr_destroy(&attr);
    GameState states[3] = {0};
    for (int i = 0; i < 3; ++i)
        start_room_client(root, i, i == 1 ? 234567 : 123456, i < 2);
    room_snapshots(states, 2500);
    for (int i = 0; i < 3; ++i)
        CHECK(player_id(i) > 0 && atomic_load(&shared->clients[i].updates) > 30);
    CHECK(player_id(0) != player_id(2) && player_id(1) == 1);
    CHECK(states[0].phase == 1 && states[2].phase == 1 && states[1].phase == 0);
    puts("PASS separate rooms retain independent snapshots and player slots");

    int victim = player_id(2) - 1;
    int x = states[2].player_x[victim];

    // Even knowing the victim's token must not bypass authenticated GUID ownership
    shared->clients[0].attack_room = 123456;
    shared->clients[0].attack_session = atomic_load(&shared->clients[2].session_id);
    command(0, SEND_INPUT, victim + 1, 'r');
    room_snapshots(states, 200);
    CHECK(states[2].player_x[victim] == x);

    // An admission in room 1 must not authorize the same player ID in room 2
    shared->clients[0].attack_room = 234567;
    shared->clients[0].attack_session = atomic_load(&shared->clients[0].session_id);
    command(0, SEND_INPUT, player_id(1), 'q');
    room_snapshots(states, 200);
    CHECK(states[1].player_active[player_id(1) - 1]);
    puts("PASS forged movement and cross-room leave cannot affect another player");

    command(2, SEND_INPUT, victim + 1, 'l');
    room_snapshots(states, 50);
    CHECK(states[2].player_x[victim] < x);
    stop_room_client(2, false);
    room_snapshots(states, 200);
    CHECK(!states[0].player_active[victim]);
    puts("PASS room owner movement and orderly leave");

    stop_room_client(1, true);
    room_snapshots(states, 3300);
    start_room_client(root, 1, 234567, true);
    room_snapshots(states, 1200);
    CHECK(player_id(1) == 1 && atomic_load(&shared->clients[1].updates) > 10);
    puts("PASS crashed room expires and its code can be reused");

    // More than 8 room codes over time must not exhaust the bounded keyed writer
    for (int cycle = 0; cycle < NET_ROOMS + 1; ++cycle)
    {
        stop_room_client(1, false);
        start_room_client(root, 1, 345678 + cycle, true);
        int64_t deadline = net_now() + INT64_C(3000000000);
        while (atomic_load(&shared->clients[1].updates) < 5 && net_now() < deadline)
            wait_ms(20);
        CHECK(player_id(1) == 1 && atomic_load(&shared->clients[1].updates) >= 5);
    }
    puts("PASS retired rooms release keyed DDS history for new room codes");
    stop_room_client(0, false);
    stop_room_client(1, false);
    for (int i = 0; i < 3; ++i)
        pthread_mutex_destroy(&shared->clients[i].snapshot_mutex);
    CHECK(munmap(shared, sizeof(*shared)) == 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--rooms") == 0)
        return test_rooms(argv[2]);

    if (argc == 2 && strcmp(argv[1], "--plain") == 0)
    {
        // Deliberately bypass net_open to model a participant that omits DDS
        // Security entirely; A secure server must never match its join writer
        dds_entity_t participant = dds_create_participant(NET_DOMAIN, NULL, NULL);
        CHECK(participant > 0);
        dds_entity_t topic = dds_create_topic(participant, &JoinRequest_desc,
                                             "JoinRequest", NULL, NULL);
        CHECK(topic > 0);

        // Match the real server's requested reliability; a QoS mismatch must
        // not be mistaken for evidence that authentication rejected this peer
        dds_qos_t *qos = dds_create_qos();
        CHECK(qos != NULL);
        dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
        dds_entity_t writer = dds_create_writer(participant, topic, qos, NULL);
        dds_delete_qos(qos);
        CHECK(writer > 0);
        dds_guid_t guid;
        CHECK(dds_get_guid(participant, &guid) == DDS_RETCODE_OK);
        JoinRequest request;
        memcpy(request.client_id, guid.v, sizeof(request.client_id));

        // Repeated writes give discovery time to finish; any reader match is
        // a failure because this participant supplied no security credentials
        for (int i = 0; i < 30; ++i)
        {
            CHECK(dds_write(writer, &request) == DDS_RETCODE_OK);
            dds_publication_matched_status_t status;
            CHECK(dds_get_publication_matched_status(writer, &status) == DDS_RETCODE_OK);
            CHECK(status.current_count == 0);
            dds_sleepfor(DDS_MSECS(100));
        }
        CHECK(dds_delete(participant) == DDS_RETCODE_OK);
        puts("PASS unauthenticated plaintext participant has no matched server reader");
        return 0;
    }

    if ((argc == 3 || argc == 4) && (strcmp(argv[1], "--probe") == 0 || strcmp(argv[1], "--link") == 0 ||
                      strcmp(argv[1], "--game") == 0))
    {
        // Use a real client process when checking admission to the game server
        Network net;
        if (!net_open(&net, false, argv[2]))
            return 1;

        // Real-server probes explicitly create or join the same test room
        net_client_room(&net, 123456, argc == 4 && strcmp(argv[3], "--create") == 0,
                        (uint64_t)net_now());
        GameState snapshot;
        bool game_test = strcmp(argv[1], "--game") == 0;
        bool link_test = strcmp(argv[1], "--probe") != 0;
        bool moved = false;
        bool results = false;
        int first_x = -1;
        int most_coins = 0;
        int64_t playing_at = 0;

        // Probe mode only needs admission plus 1 valid snapshot; link mode
        // stays active for 8 seconds to observe delivery under impairment
        uint64_t gaps = 0;
        uint64_t previous = 0;
        int64_t last_received = 0;
        int64_t intervals[512];
        int interval_count = 0;
        // The game check stays for a full round; this exercises the actual
        // server adapter rather than the deterministic protocol fixture
        int64_t deadline = net_now() + (game_test ? INT64_C(65000000000) :
            link_test ? INT64_C(8000000000) : INT64_C(5000000000));
        while (net_now() < deadline && (link_test || net.received == 0))
        {
            if (net_client_state(&net, &snapshot))
            {
                int64_t received = net_now();
                if (last_received && interval_count < 512)
                    intervals[interval_count++] = received - last_received;
                last_received = received;

                // Count only gaps after the first received snapshot; sequences
                // published before admission are not losses experienced by us
                if (previous && snapshot.seq > previous + 1)
                    gaps += snapshot.seq - previous - 1;
                previous = snapshot.seq;

                if (game_test && net.player_id > 0)
                {
                    int x = snapshot.player_x[net.player_id - 1];
                    if (first_x == -1)
                        first_x = x;
                    moved |= x != first_x;
                    int coins = 0;
                    for (int i = 0; i < NET_COINS; ++i)
                        coins += snapshot.coin_active[i];
                    if (coins > most_coins)
                        most_coins = coins;
                    if (snapshot.phase == 1 && playing_at == 0)
                        playing_at = received;

                    // Check the transmitted winner against the same snapshot's
                    // scores; the bitmask retains every tied leader
                    if (snapshot.phase == 2 && !results)
                    {
                        CHECK(playing_at != 0);
                        CHECK(received - playing_at >= INT64_C(59000000000));
                        CHECK(received - playing_at <= INT64_C(62000000000));
                        int best = -1, winners = 0;
                        for (int i = 0; i < NET_PLAYERS; ++i)
                        {
                            if (!snapshot.player_active[i])
                                continue;
                            if (snapshot.player_score[i] > best)
                            {
                                best = snapshot.player_score[i];
                                winners = 1 << i;
                            }
                            else if (snapshot.player_score[i] == best)
                                winners |= 1 << i;
                        }
                        CHECK(snapshot.winners == winners);
                        CHECK(snapshot.best_score == best);
                        results = true;
                    }
                }
            }
            CHECK(net_client_send(&net, link_test ? 'r' : 'n'));
            dds_sleepfor(DDS_MSECS(1));
        }

        // An admission alone does not prove communication works in both
        // directions; even the short probe must receive a validated snapshot
        // Exit 1 means startup failed; exit 2 means no usable connection within
        // the test window; the external checks expect these for rejected peers
        int result = net.player_id > 0 && net.received >= (link_test ? 30u : 1u) ? 0 : 2;
        if (game_test)
        {
            CHECK(moved && results && most_coins > 0);
            printf("PASS actual game transport: movement, %d coins and full-round results\n", most_coins);
        }
        printf("Client player ID: %d, snapshots: %llu, gaps: %llu\n", net.player_id,
               (unsigned long long)net.received, (unsigned long long)gaps);

        // These are arrival intervals, not one-way network latency; they need
        // no clock synchronisation and are also useful on separate computers
        if (interval_count > 0)
        {
            int64_t total = 0;
            for (int i = 0; i < interval_count; ++i)
                total += intervals[i];
            qsort(intervals, (size_t)interval_count, sizeof(intervals[0]), compare_time);
            printf("SNAPSHOT intervals: mean %.3f ms, p95 %.3f ms, max %.3f ms\n",
                   total / (interval_count * 1e6),
                   intervals[(interval_count * 95 + 99) / 100 - 1] / 1e6,
                   intervals[interval_count - 1] / 1e6);
        }
        net_close(&net);
        return result;
    }

    const char *root = argc > 1 ? argv[1] : "credentials";

    // Ordinary globals become private copies after fork; MAP_SHARED keeps the
    // command/report area visible to every process without sharing DDS handles
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    memset(shared, 0, sizeof(*shared));
    for (int i = 0; i < 6; ++i)
        atomic_store(&shared->clients[i].player_id, -1);

    // Register cleanup before the first fork so partial startup failures do not
    // leave running participants behind; DDS threads are created only in children
    parent_pid = getpid();
    atexit(cleanup);
    fflush(NULL);
    for (int i = 0; i < 7; ++i)
    {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0)
        {
            if (i == 0)
                test_server(root);
            else
                test_client(root, i - 1);
        }
    }

    // Admit 2 clients together; distinct IDs and actual payload checks prove
    // the replies were routed correctly and state travelled back to both clients
    atomic_store(&shared->clients[0].start, true);
    atomic_store(&shared->clients[1].start, true);
    wait_ms(2500);
    CHECK(atomic_load(&shared->server_ready) == 1);
    CHECK(atomic_load(&shared->joins) == 2 && player_id(0) > 0 && player_id(1) > 0);
    CHECK(player_id(0) != player_id(1));
    CHECK(atomic_load(&shared->clients[0].updates) > 0);
    CHECK(atomic_load(&shared->clients[1].updates) > 0);
    CHECK(atomic_load(&shared->clients[0].payload_checks) > 0);
    puts("PASS simultaneous joins and complete snapshots");

    // Repeated admission requests must return the existing assignment rather
    // than consume 3 more slots; the cumulative join count must stay at 2
    for (int i = 0; i < 3; ++i)
        command(0, RETRY_JOIN, 0, 0);
    wait_ms(100);
    CHECK(atomic_load(&shared->joins) == 2);
    puts("PASS repeated join requests");

    // Send client 1's claimed GUID through client 0's authenticated participant
    // A rise in rejected confirms the server checked DDS metadata against it
    uint64_t before = atomic_load(&shared->rejected);
    memcpy(shared->clients[0].forged_guid, shared->clients[1].guid, 16);
    command(0, FORGE_JOIN, 0, 0);
    wait_ms(50);
    CHECK(atomic_load(&shared->rejected) > before);
    puts("PASS forged join identity rejected");

    int id = player_id(0);
    int old_moves = atomic_load(&shared->moves);
    before = atomic_load(&shared->rejected);

    // Cover below-range IDs, above-range IDs, a free slot and another live slot
    // None may produce movement; the 2 bad directions add 2 more rejections
    int invalid_ids[] = {0, -1, 6, INT_MAX, 5, player_id(1)};
    for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++i)
    {
        command(0, SEND_INPUT, invalid_ids[i], 'r');
        wait_ms(30);
    }
    command(0, SEND_INPUT, id, 'x');
    wait_ms(30);
    command(0, SEND_INPUT, id, '\0');
    wait_ms(30);
    CHECK(atomic_load(&shared->moves) == old_moves);
    CHECK(atomic_load(&shared->rejected) >= before + 8);
    puts("PASS invalid IDs, inactive slot, invalid directions and impersonation rejected");

    // Establish 1 accepted movement first; replaying that exact sequence must
    // increase rejections without increasing the validated movement count
    command(0, SEND_INPUT, id, 'r');
    wait_ms(40);
    CHECK(atomic_load(&shared->moves) == old_moves + 1);
    before = atomic_load(&shared->rejected);
    command(0, REPLAY, id, 'r');
    wait_ms(40);
    CHECK(atomic_load(&shared->moves) == old_moves + 1);
    CHECK(atomic_load(&shared->rejected) > before);
    puts("PASS valid movement and replay rejection");

    old_moves = atomic_load(&shared->moves);
    before = atomic_load(&shared->rejected);

    // Judge accepted movement against elapsed server time, not the number of
    // writes; bounded DDS history can discard old samples during the flood
    int64_t started = net_now();
    command(0, FLOOD, id, 'r');
    wait_ms(100);
    int64_t elapsed = net_now() - started;
    int accepted = atomic_load(&shared->moves) - old_moves;
    CHECK(accepted <= elapsed / NET_INPUT_NS + 1);
    CHECK(atomic_load(&shared->rejected) > before);
    printf("PASS flood: 2000 sends, %d moves in %.2f ms\n", accepted, elapsed / 1e6);

    command(0, FORGE_STATE, 0, 0);
    puts("PASS client cannot publish state/replies or subscribe to raw input");

    // Wait for each rejection before sending the next malformed sample so a
    // depth-1 reader cannot replace it before the client has a chance to inspect it
    for (int kind = 1; kind <= 19; ++kind)
    {
        before = atomic_load(&shared->clients[0].rejected);
        atomic_store(&shared->bad_snapshot, kind);
        int64_t deadline = net_now() + INT64_C(1000000000);
        while (atomic_load(&shared->clients[0].rejected) == before && net_now() < deadline)
            wait_ms(1);
        CHECK(atomic_load(&shared->clients[0].rejected) > before);
    }

    // Invalid high-sequence snapshots must not poison the client's sequence
    // tracking; later valid snapshots should still advance its update count
    int old_updates = atomic_load(&shared->clients[0].updates);
    wait_ms(150);
    CHECK(atomic_load(&shared->clients[0].updates) > old_updates);
    puts("PASS 15 malformed snapshot cases, repeated snapshot and recovery");

    // Both processes use this host's monotonic clock; measure DDS write to
    // validated server event, excluding snapshots, rendering and parent polling
    // Space samples beyond the movement limiter so rate rejection cannot skew it
    int64_t latency[200];
    int64_t total = 0;
    for (int i = 0; i < 200; ++i)
    {
        wait_ms(12);
        old_moves = atomic_load(&shared->moves);
        int client = i % 2;
        command(client, SEND_INPUT, player_id(client), 'r');
        int64_t deadline = net_now() + INT64_C(1000000000);
        while (atomic_load(&shared->moves) == old_moves && net_now() < deadline)
            wait_ms(1);
        CHECK(atomic_load(&shared->moves) == old_moves + 1);

        latency[i] = atomic_load(&shared->received_at) -
                     atomic_load(&shared->clients[client].sent_at);
        total += latency[i];
    }

    // With 200 sorted samples, index 189 is the nearest-rank 95th percentile
    qsort(latency, 200, sizeof(latency[0]), compare_time);
    printf("BENCH 200 local inputs: mean %.3f ms, p95 %.3f ms, max %.3f ms\n",
           total / 200e6, latency[189] / 1e6, latency[199] / 1e6);

    // Reuse the same DDS participant and numeric slot after expiry; A fresh
    // sequence with the previous token must still fail; sequence checks alone
    // cannot distinguish 2 admissions belonging to the same participant
    command(0, SAVE_SESSION, 0, 0);
    uint64_t old_session = atomic_load(&shared->clients[0].session_id);
    atomic_store(&shared->clients[0].pause, true);

    // Wait beyond the 3-second lease; snapshots should tell this still-live
    // client that its slot is inactive even though its sends are paused
    wait_ms(3400);
    CHECK(player_id(0) == -1);
    atomic_store(&shared->clients[0].pause, false);
    int64_t session_deadline = net_now() + INT64_C(3000000000);
    while (player_id(0) == -1 && net_now() < session_deadline)
        wait_ms(1);
    CHECK(player_id(0) == id);
    CHECK(atomic_load(&shared->clients[0].session_id) != old_session);
    old_moves = atomic_load(&shared->moves);
    before = atomic_load(&shared->rejected);
    command(0, OLD_SESSION, id, 'r');
    wait_ms(50);
    CHECK(atomic_load(&shared->moves) == old_moves);
    CHECK(atomic_load(&shared->rejected) > before);
    command(0, SEND_INPUT, id, 'r');
    wait_ms(50);
    CHECK(atomic_load(&shared->moves) == old_moves + 1);
    puts("PASS old admission token rejected after same-participant rejoin");

    // 2 players are active again; start 4 more contenders for 3 slots
    // Use a relative join count because the earlier rejoin also counted as a join
    int previous_joins = atomic_load(&shared->joins);
    for (int i = 2; i < 6; ++i)
        atomic_store(&shared->clients[i].start, true);
    wait_ms(2500);
    CHECK(atomic_load(&shared->joins) == previous_joins + 3);

    // Identify whichever of the 4 simultaneous clients was refused
    int waiting = -1;
    for (int i = 2; i < 6; ++i)
    {
        CHECK(atomic_load(&shared->clients[i].ready) == 1);
        if (player_id(i) == -1)
        {
            CHECK(waiting == -1);
            waiting = i;
        }
    }
    CHECK(waiting >= 0);
    puts("PASS 5-player capacity and full-server rejection");

    int dropped_id = player_id(0);
    int old_leaves = atomic_load(&shared->leaves);

    // SIGKILL skips net_close completely, like a crash or powered-off machine
    // The remaining client must take the released slot without restarting DDS
    CHECK(kill(children[1], SIGKILL) == 0);
    int crash_status;
    CHECK(waitpid(children[1], &crash_status, 0) == children[1]);
    CHECK(WIFSIGNALED(crash_status) && WTERMSIG(crash_status) == SIGKILL);
    children[1] = 0;
    wait_ms(3800);
    CHECK(atomic_load(&shared->leaves) == old_leaves + 1);
    CHECK(player_id(waiting) == dropped_id);
    puts("PASS crashed client heartbeat timeout and slot reuse");

    // Normal close sends q before deleting DDS; removal should happen well
    // before the 3-second heartbeat timeout used by the crash test
    old_leaves = atomic_load(&shared->leaves);
    command(1, CLOSE, 0, 0);
    wait_ms(400);
    CHECK(atomic_load(&shared->leaves) == old_leaves + 1);
    puts("PASS graceful leave");

    // Stop cleanly and reap the server before measuring client timeout; keeping
    // clients alive checks they clear old assignments without being restarted
    atomic_store(&shared->stop_server, true);
    int status;
    CHECK(waitpid(children[0], &status, 0) == children[0]);
    children[0] = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    wait_ms(3300);
    CHECK(player_id(waiting) == -1);
    puts("PASS server timeout clears client assignment");

    // Start a new server participant and let the surviving clients join it
    atomic_store(&shared->server_ready, 0);
    atomic_store(&shared->stop_server, false);

    // Normal exit lets sanitizer builds inspect child cleanup; flush before
    // fork so those exits cannot print a second copy of the parent's output
    fflush(NULL);
    children[0] = fork();
    CHECK(children[0] >= 0);
    if (children[0] == 0)
        test_server(root);

    int64_t reconnect_deadline = net_now() + INT64_C(5000000000);
    bool reconnected = false;
    while (!reconnected && net_now() < reconnect_deadline)
    {
        reconnected = true;
        for (int i = 2; i < 6; ++i)
            if (player_id(i) < 1)
                reconnected = false;
        wait_ms(10);
    }
    CHECK(reconnected);

    // Every survivor needs its own slot on the new server; this bitmask catches
    // duplicate assignments even when all clients report a positive player ID
    unsigned int slots = 0;
    for (int i = 2; i < 6; ++i)
    {
        unsigned int bit = 1u << player_id(i);
        CHECK((slots & bit) == 0);
        slots |= bit;
    }
    puts("PASS automatic reconnect after server restart");

    atomic_store(&shared->stop_server, true);
    CHECK(waitpid(children[0], &status, 0) == children[0]);
    children[0] = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Stop all client processes before opening another test participant
    for (int i = 0; i < 6; ++i)
    {
        if (children[i + 1] == 0)
            continue;
        atomic_store(&shared->clients[i].start, true);
        atomic_store(&shared->clients[i].stop, true);
        CHECK(waitpid(children[i + 1], &status, 0) == children[i + 1]);
        children[i + 1] = 0;
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Missing keys must fail closed; there must be no automatic insecure startup
    // Perform this after participant cleanup so it cannot interfere with discovery
    Network missing;
    CHECK(!net_open(&missing, false, "build/no-such-credentials"));
    puts("PASS missing credentials rejected");
    munmap(shared, sizeof(*shared));
    puts("All checks passed");
    return 0;
}
