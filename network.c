#define _POSIX_C_SOURCE 200809L
#include "network.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/random.h>
#include <unistd.h>

int64_t net_now(void)
{
    // Wall-clock corrections must not extend a session or bypass movement limits
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static bool checked(dds_entity_t result, const char *operation)
{
    // DDS returns negative error codes; keep the operation in the diagnostic so
    // missing plugins and denied endpoints are distinguishable during setup
    if (result >= 0)
        return true;

    fprintf(stderr, "%s: %s\n", operation, dds_strretcode(-result));
    return false;
}

static bool same_id(const dds_guid_t *a, const dds_guid_t *b)
{
    return memcmp(a->v, b->v, sizeof(a->v)) == 0;
}

static bool credential(dds_qos_t *qos, const char *property,
                       const char *directory, const char *filename)
{
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, filename);

    // Stop if the path was cut short or the credential cannot be read
    if (length < 0 || (size_t)length >= sizeof(path) || access(path, R_OK) != 0)
    {
        fprintf(stderr, "Cannot read credential: %s/%s\n", directory, filename);
        return false;
    }

    char uri[4101];
    snprintf(uri, sizeof(uri), "file:%s", path);
    dds_qset_prop(qos, property, uri);
    return true;
}

static dds_entity_t endpoint(Network *net, const dds_topic_descriptor_t *descriptor,
                             const char *name, bool writer, bool reliable, int depth)
{
    dds_entity_t topic = dds_create_topic(net->participant, descriptor, name, NULL, NULL);
    if (!checked(topic, name))
        return topic;

    dds_qos_t *qos = dds_create_qos();
    if (!qos)
        return DDS_RETCODE_OUT_OF_RESOURCES;

    // All topics are unkeyed, so this is a shared bounded history, not a queue per
    // player; it bounds application backlog; it does not prevent network DoS
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, depth);
    dds_qset_resource_limits(qos, depth, 1, depth);
    dds_qset_reliability(qos, reliable ? DDS_RELIABILITY_RELIABLE :
                         DDS_RELIABILITY_BEST_EFFORT, DDS_MSECS(100));

    // A late joiner must not receive an old admission or an old player's input
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);

    dds_entity_t result = writer ? dds_create_writer(net->participant, topic, qos, NULL) :
                                  dds_create_reader(net->participant, topic, qos, NULL);
    dds_delete_qos(qos);
    checked(result, name);
    return result;
}

bool net_participant(Network *net, bool server, const char *credentials)
{
    memset(net, 0, sizeof(*net));
    net->server = server;
    net->player_id = -1;

    dds_qos_t *qos = dds_create_qos();
    if (!qos)
        return false;

    // Authentication checks the certificate, access control checks topic rights
    // and crypto protects the traffic; all 3 plugins are required; startup
    // fails if any plugin or signed policy cannot be loaded
    dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
    dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
    dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");
    dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
    dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
    dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");
    dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

    bool ready = credential(qos, "dds.sec.auth.identity_ca", credentials, "ca.pem") &&
        credential(qos, "dds.sec.auth.identity_certificate", credentials, "identity.pem") &&
        credential(qos, "dds.sec.auth.private_key", credentials, "identity.key") &&
        credential(qos, "dds.sec.access.permissions_ca", credentials, "ca.pem") &&
        credential(qos, "dds.sec.access.governance", credentials, "governance.p7s") &&
        credential(qos, "dds.sec.access.permissions", credentials, "permissions.p7s");

    if (ready)
        net->participant = dds_create_participant(NET_DOMAIN, qos, NULL);

    dds_delete_qos(qos);
    if (!ready || !checked(net->participant, "Create secure participant"))
        return false;

    if (!checked(dds_get_guid(net->participant, &net->own_id), "Get participant GUID"))
        goto fail;

    net->last_state = net_now();
    return true;

fail:
    net_close(net);
    return false;
}

bool net_open(Network *net, bool server, const char *credentials)
{
    if (!net_participant(net, server, credentials))
        return false;

    // Joins and input use reliable delivery with a finite writer blocking time
    // Snapshots use best effort and depth 1; receiving a fresh complete state is
    // more useful than retransmitting positions that have already been replaced
    net->join = endpoint(net, &JoinRequest_desc, "JoinRequest", !server, true, 32);
    net->response = endpoint(net, &JoinResponse_desc, "JoinResponse", server, true, 32);
    net->input = endpoint(net, &Input_desc, "Input", !server, true, 32);
    net->state = endpoint(net, &GameState_desc, "GameState", server, false, 1);

    if (net->join < 0 || net->response < 0 || net->input < 0 || net->state < 0)
        goto fail;

    return true;

fail:
    net_close(net);
    return false;
}

static bool sender(dds_entity_t reader, const dds_sample_info_t *info, dds_guid_t *id)
{
    // Get the sender from DDS metadata instead of trusting an ID in the message
    dds_builtintopic_endpoint_t *data =
        dds_get_matched_publication_data(reader, info->publication_handle);
    if (!data)
        return false;

    *id = data->participant_key;
    dds_builtintopic_free_endpoint(data);
    return true;
}

static bool take(dds_entity_t reader, void *sample, dds_sample_info_t *info)
{
    // Supply storage because these IDL types contain only fixed-size fields
    // DDS disposal notifications have no payload; never interpret them as input
    void *samples[] = {sample};
    dds_return_t count = dds_take(reader, samples, info, 1, 1);
    if (count < 0)
        checked(count, "Read DDS sample");
    return count == 1 && info->valid_data;
}

bool net_server_accept(Network *net, int player_id)
{
    if (player_id < 0 || player_id > NET_PLAYERS)
        return false;

    // A numeric player ID is reused after a disconnect; bind the new admission to
    // both its DDS sender and a random token; otherwise a delayed old input from
    // the same participant could become valid again when it rejoins that slot
    if (player_id != 0)
    {
        NetSession *session = &net->sessions[player_id - 1];
        if (session->active)
            return false;

        uint64_t token;
        ssize_t bytes;
        do
        {
            bytes = getrandom(&token, sizeof(token), 0);
        } while (bytes < 0 && errno == EINTR);

        // Never fall back to rand or a clock value for an admission token
        if (bytes != sizeof(token) || token == 0)
        {
            fprintf(stderr, "Cannot generate session token\n");
            return false;
        }

        *session = (NetSession){.active = true, .owner = net->pending_id,
                               .session_id = token, .last_seen = net_now()};
    }

    JoinResponse response = {.int_player_id = player_id};
    if (player_id > 0)
        response.session_id = net->sessions[player_id - 1].session_id;
    memcpy(response.client_id, net->pending_id.v, sizeof(response.client_id));
    return checked(dds_write(net->response, &response), "Send join reply");
}

NetEvent net_server_expire(Network *net)
{
    int64_t now = net_now();
    for (int i = 0; i < NET_PLAYERS; ++i)
    {
        if (net->sessions[i].active && now - net->sessions[i].last_seen >= NET_TIMEOUT_NS)
        {
            net->sessions[i].active = false;
            return (NetEvent){NET_LEAVE, i + 1, 'q'};
        }
    }
    return (NetEvent){0};
}

bool net_server_join(Network *net, const JoinRequest *request, const dds_sample_info_t *info)
{
    dds_guid_t owner;

    // Validate the already-read sample so the game server can keep its DDS loop
    // Repeated joins reply with the existing slot instead of allocating another
    if (!info->valid_data || !sender(net->join, info, &owner) ||
        memcmp(request->client_id, owner.v, sizeof(request->client_id)) != 0)
    {
        ++net->rejected;
        return false;
    }

    net->pending_id = owner;
    for (int i = 0; i < NET_PLAYERS; ++i)
    {
        if (net->sessions[i].active && same_id(&owner, &net->sessions[i].owner))
        {
            JoinResponse response = {.int_player_id = i + 1,
                .session_id = net->sessions[i].session_id};
            memcpy(response.client_id, owner.v, sizeof(response.client_id));
            checked(dds_write(net->response, &response), "Resend join reply");
            return false;
        }
    }
    return true;
}

NetEvent net_server_input(Network *net, const Input *input, const dds_sample_info_t *info)
{
    int64_t now = net_now();
    dds_guid_t owner;
    ++net->received;
    bool valid = info->valid_data && input->int_player_id >= 1 &&
        input->int_player_id <= NET_PLAYERS && input->ch_direction != '\0' &&
        strchr("lrnq", input->ch_direction) != NULL;
    NetSession *session = valid ? &net->sessions[input->int_player_id - 1] : NULL;

    // Do not index an unchecked player ID or refresh a lease for forged input
    if (!valid || !session->active || !sender(net->input, info, &owner) ||
        !same_id(&owner, &session->owner) || input->session_id != session->session_id ||
        input->seq <= session->seq)
    {
        ++net->rejected;
        return (NetEvent){0};
    }

    // Consume the sequence even when a movement exceeds the rate limit
    session->seq = input->seq;
    session->last_seen = now;
    if (input->ch_direction == 'q')
    {
        session->active = false;
        return (NetEvent){NET_LEAVE, input->int_player_id, 'q'};
    }
    if (input->ch_direction == 'n')
        return (NetEvent){NET_HEARTBEAT, input->int_player_id, 'n'};
    if (now - session->last_move >= NET_INPUT_NS)
    {
        session->last_move = now;
        return (NetEvent){NET_MOVE, input->int_player_id, input->ch_direction};
    }
    ++net->rejected;
    return (NetEvent){0};
}

NetEvent net_server_poll(Network *net)
{
    NetEvent event = net_server_expire(net);
    if (event.type != NET_NONE)
        return event;

    Input input;
    dds_sample_info_t info;
    if (take(net->input, &input, &info))
    {
        event = net_server_input(net, &input, &info);
        if (event.type == NET_MOVE || event.type == NET_LEAVE)
            return event;
    }

    JoinRequest request;
    if (take(net->join, &request, &info) && net_server_join(net, &request, &info))
        return (NetEvent){NET_JOIN, 0, 0};
    return (NetEvent){0};
}

bool net_server_state(Network *net, GameState *state)
{
    // Give each complete snapshot a seq so clients can discard old state
    state->seq = ++net->state_sequence;
    return checked(dds_write(net->state, state), "Send game state");
}

bool net_client_send(Network *net, char direction)
{
    int64_t now = net_now();

    if (now - net->last_state >= NET_TIMEOUT_NS)
    {
        // Clear the old server after a timeout and start joining again
        net->player_id = -1;
        net->session_id = 0;
        net->server_id = (dds_guid_t){0};
        net->state_sequence = 0;
    }

    if (net->player_id == -1)
    {
        if (now - net->last_join < INT64_C(500000000))
            return true;

        JoinRequest request;
        memcpy(request.client_id, net->own_id.v, sizeof(request.client_id));
        net->last_join = now;
        return checked(dds_write(net->join, &request), "Send join request");
    }

    // Send idle heartbeats every 100 ms and movement no faster than every 10 ms
    int64_t interval = direction == 'n' ? INT64_C(100000000) : NET_INPUT_NS;
    if (direction != 'q' && now - net->last_input < interval)
        return true;

    Input input = {net->player_id, direction, ++net->seq, net->session_id};
    net->last_input = now;
    return checked(dds_write(net->input, &input), "Send input");
}

static bool valid_state(const GameState *state)
{
    // Even authenticated data is checked before the client uses it for rendering; keep
    // these transport bounds aligned with the IDL contract; do not decide game
    // rules here, such as who wins or whether a disconnected score is retained
    if (state->state < 0 || state->state > 2 || state->remaining_ms < 0 ||
        state->remaining_ms > 60000 || state->winner_id < 0 || state->winner_id > NET_PLAYERS)
        return false;

    for (int i = 0; i < NET_PLAYERS; ++i)
    {
        const PlayerState *player = &state->players[i];
        if (player->x < 0 || player->x > 750 || player->y < 0 || player->y > 550 ||
            player->score < 0)
            return false;
    }

    for (int i = 0; i < NET_COINS; ++i)
    {
        // Spawning uses a top-left corner above the arena; after conversion,
        // the centre can extend past either vertical edge by the coin's half-size
        if (state->coins[i].x < 0 || state->coins[i].x > 800 ||
            state->coins[i].y < -620 || state->coins[i].y > 620)
            return false;
    }
    return true;
}

bool net_client_state(Network *net, GameState *state)
{
    JoinResponse response;
    dds_sample_info_t info;
    dds_guid_t owner;

    if (take(net->response, &response, &info) && net->player_id == -1 &&
        memcmp(response.client_id, net->own_id.v, sizeof(response.client_id)) == 0 &&
        response.int_player_id >= 1 && response.int_player_id <= NET_PLAYERS &&
        response.session_id != 0 &&
        sender(net->response, &info, &owner))
    {
        net->player_id = response.int_player_id;
        net->session_id = response.session_id;
        net->server_id = owner;
        net->state_sequence = 0;
        net->last_state = net_now();
    }

    GameState sample;
    if (!take(net->state, &sample, &info) || net->player_id == -1)
        return false;

    // Only accept snapshots from the server that assigned this player slot
    if (!sender(net->state, &info, &owner) || !same_id(&owner, &net->server_id) ||
        sample.seq <= net->state_sequence || !valid_state(&sample))
    {
        ++net->rejected;
        return false;
    }

    // Commit only after every check passes; malformed data must not partially
    // replace the display or keep a dead connection alive
    *state = sample;
    net->state_sequence = sample.seq;
    net->last_state = net_now();
    ++net->received;

    // Rejoin if the server no longer includes this player in its active slots
    if (!state->players[net->player_id - 1].active)
    {
        net->player_id = -1;
        net->session_id = 0;
    }

    return true;
}

void net_close(Network *net)
{
    if (net->participant <= 0)
        return;

    if (!net->server && net->player_id > 0)
    {
        // Let the server acknowledge the leave before deleting this writer
        Input leave = {net->player_id, 'q', ++net->seq, net->session_id};
        if (dds_write(net->input, &leave) == DDS_RETCODE_OK)
            dds_wait_for_acks(net->input, DDS_MSECS(200));
    }

    dds_delete(net->participant);
    net->participant = 0;
}
