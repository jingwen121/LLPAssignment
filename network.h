#ifndef COIN_RUSH_NETWORK_H
#define COIN_RUSH_NETWORK_H

#include <stdbool.h>
#include <stdint.h>
#include "dds/dds.h"
#include "messages.h"

#define NET_PLAYERS 5
#define NET_COINS 5
#define NET_DOMAIN 42
#define NET_TIMEOUT_NS INT64_C(3000000000)
#define NET_INPUT_NS INT64_C(10000000)

typedef enum { NET_NONE, NET_JOIN, NET_MOVE, NET_LEAVE } NetEventType;

typedef struct
{
    NetEventType type;
    int player_id;
    char direction;
} NetEvent;

typedef struct
{
    bool active;

    // Keep the DDS participant GUID that owns this player slot
    dds_guid_t owner;

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
    uint64_t seq;
    uint64_t state_sequence;
    int64_t last_join;
    int64_t last_input;
    int64_t last_state;
    uint64_t rejected;
    uint64_t received;
    bool server;
} Network;

// Keep all calls for one Network on the same worker thread
bool net_open(Network *net, bool server, const char *credentials);
void net_close(Network *net);
int64_t net_now(void);

// Read one event and let the game choose a slot for a new player
NetEvent net_server_poll(Network *net);
bool net_server_accept(Network *net, int player_id);
bool net_server_state(Network *net, GameState *state);

// Retry joins and send input or heartbeats without depending on the render rate
bool net_client_send(Network *net, char direction);
bool net_client_state(Network *net, GameState *state);

#endif
