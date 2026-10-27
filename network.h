#ifndef COIN_RUSH_NETWORK_H
#define COIN_RUSH_NETWORK_H

#include <stdbool.h>
#include <stdint.h>
#include "dds/dds.h"
#include "messages.h"

#define NET_PLAYERS 5
#define NET_COINS 5
#define NET_DOMAIN 42

// These are monotonic nanoseconds; neither timeout depends on the wall clock
// The server applies the movement limit even if a client removes its own limiter
#define NET_TIMEOUT_NS INT64_C(3000000000)
#define NET_INPUT_NS INT64_C(10000000)

typedef enum { NET_NONE, NET_JOIN, NET_MOVE, NET_LEAVE } NetEventType;

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

    // Save the last accepted input so older inputs cannot be replayed
    uint64_t seq;

    // Store monotonic times so changing the system clock does not affect timeouts
    int64_t last_seen;
    int64_t last_move;
} NetSession;

typedef struct
{
    dds_entity_t participant;
    dds_entity_t join;
    dds_entity_t response;
    dds_entity_t input;
    dds_entity_t state;
    dds_guid_t own_id;

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
    bool server;
} Network;

// Keep all calls for 1 Network on the same worker thread; the caller owns this
// object; render/game threads exchange copies under their mutex, never DDS handles
// net_open requires readable signed credentials; a failure must stop the worker
bool net_open(Network *net, bool server, const char *credentials);
void net_close(Network *net);
int64_t net_now(void);

// Read 1 event and let the game choose a slot for a new player; handle NET_JOIN
// with net_server_accept before polling again; 0 replies that the server is full
// NET_LEAVE is emitted once for a valid leave or expired heartbeat; the server clears the
// player and decides what that means for an in-progress round
NetEvent net_server_poll(Network *net);
bool net_server_accept(Network *net, int player_id);
bool net_server_state(Network *net, GameState *state);

// Retry joins and send input or heartbeats without depending on the render rate
// Poll state and call send regularly on the same worker; false from state can
// simply mean there is no new valid snapshot; false from send is a write failure
bool net_client_send(Network *net, char direction);
bool net_client_state(Network *net, GameState *state);

#endif
