#ifndef COIN_RUSH_NETWORK_H
#define COIN_RUSH_NETWORK_H

#include <stdbool.h>
#include <stdint.h>
#include "dds/dds.h"
#include <messages.h>

#define NET_PLAYERS 5
// Capacity covers the current game without changing how many coins it spawns
#define NET_COINS 8
#define NET_BOMBS 4
#define NET_DOMAIN 42
#define NET_ROOMS 8
#define NET_IDENTITIES 6
#define NET_QUICKPLAY_ROOM_CODE 100000

// These are monotonic nanoseconds; neither timeout depends on the wall clock
// The server applies the movement limit even if a client removes its own limiter
#define NET_TIMEOUT_NS INT64_C(3000000000)
#define NET_INPUT_NS INT64_C(10000000)

typedef enum { NET_NONE, NET_JOIN, NET_MOVE, NET_LEAVE, NET_HEARTBEAT, NET_CONTINUE } NetEventType;

typedef struct
{
    // Only validated events cross into game code; raw DDS samples stay here
    NetEventType type;
    int player_id;
    char direction;
} NetEvent;

typedef struct
{
    bool active;

    // Keep the DDS participant GUID that owns this player slot
    dds_guid_t owner;

    // Ownership survives a retry, but a new admission must not reuse an old token
    uint64_t session_id;
    uint64_t client_token;
    int identity;

    // Save the last accepted input so older inputs cannot be replayed
    uint64_t seq;

    // Store monotonic times so changing the system clock does not affect timeouts
    int64_t last_seen;
    int64_t last_move;
} NetSession;

// Shared by room copies; limits follow the signed identity rather than a process
// A new participant using the same certificate does not get a fresh quota
typedef struct
{
    dds_guid_t owner;
    int32_t room_code;
    int64_t expires;
    int64_t next_join;
} NetAdmission;

typedef struct
{
    dds_entity_t participant;
    dds_entity_t join;
    dds_entity_t response;
    dds_entity_t input;
    dds_entity_t state;
    dds_guid_t own_id;
    dds_entity_t join_readers[NET_IDENTITIES];
    dds_entity_t replies[NET_IDENTITIES];
    dds_entity_t states[NET_IDENTITIES];
    unsigned next_reader;
    int identity;
    int pending_identity;
    NetAdmission admission_storage[NET_IDENTITIES];
    NetAdmission *admissions;

    // Accept state only from the server that assigned this client's player ID
    dds_guid_t server_id;

    // Keep the join sender while the game thread chooses its slot
    dds_guid_t pending_id;
    NetSession sessions[NET_PLAYERS];
    int player_id;

    // Client copy of its current admission token; 0 means no accepted admission
    uint64_t session_id;

    // Input sequences keep increasing across reconnects; snapshot sequences are
    // scoped to the assigning server; a newly accepted server starts that check over
    uint64_t seq;
    uint64_t state_sequence;
    int64_t last_join;
    int64_t last_input;
    int64_t last_state;

    // Diagnostic counters for QA; server received counts raw inputs, while client
    // received counts validated snapshots; rejected also includes rate limiting
    uint64_t rejected;
    uint64_t received;
    // Room routing is checked alongside GUID ownership; the room code alone
    // never authorizes movement or reveals another player's admission token
    int32_t room_code;
    bool create_room;
    char display_name[17];
    uint64_t client_token;
    int32_t pending_room;
    uint64_t pending_token;
    bool server;
} Network;

// Keep all calls for 1 Network on the same worker thread; the caller owns this
// object; render/game threads exchange copies under their mutex, never DDS handles
// net_open requires readable signed credentials; a failure must stop the worker
bool net_open(Network *net, bool server, const char *credentials);
// Create only the secure participant when the caller owns its existing endpoints
bool net_participant(Network *net, bool server, const char *credentials);
void net_close(Network *net);
int64_t net_now(void);
bool net_valid_name(const char name[17]);

// Read 1 event and let the game choose a slot for a new player; handle NET_JOIN
// with net_server_accept before polling again; 0 replies that the server is full
// NET_LEAVE is emitted once for a valid leave or expired heartbeat; the server clears the
// player and decides what that means for an in-progress round
NetEvent net_server_poll(Network *net);
// Apply the same checks to samples read by an existing server receive loop
// Store its reader/writer handles in Network before calling these helpers
NetEvent net_server_expire(Network *net);
bool net_server_join(Network *net, const JoinRequest *request, const dds_sample_info_t *info);
NetEvent net_server_input(Network *net, const Input *input, const dds_sample_info_t *info);
dds_return_t net_server_take_join(Network *net, void **samples,
    dds_sample_info_t *info, size_t size, uint32_t count);
void net_server_retire(Network *net, const GameState *state);
bool net_server_accept(Network *net, int player_id);
bool net_server_state(Network *net, GameState *state);

// Retry joins and send input or heartbeats without depending on the render rate
// Poll state and call send regularly on the same worker; false from state can
// simply mean there is no new valid snapshot; false from send is a write failure
// Select a room before sending its first join; 0 is reserved for the protocol fixture
void net_client_room(Network *net, int32_t room_code, bool create_room, uint64_t token);
bool net_client_send(Network *net, char direction);
bool net_client_state(Network *net, GameState *state);

#endif
