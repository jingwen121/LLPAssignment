#define _POSIX_C_SOURCE 200809L
#include "network.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/random.h>
#include <unistd.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

int64_t net_now(void)
{
    // Wall-clock corrections must not extend a session or bypass movement limits
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

bool net_valid_name(const char name[17])
{
    for (int i = 0; i < 17; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0) return true;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-')) return false;
    }
    return false;
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

    // State is keyed by room; reserve 1 latest snapshot per room so a busy
    // room cannot evict another room's update; other topics share bounded queues
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, depth);
    int instances = descriptor == &GameState_desc ? NET_ROOMS : 1;
    dds_qset_resource_limits(qos, depth * instances, instances, depth);
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
    net->admissions = net->admission_storage;

    // The certificate chooses the channel; the signed DDS grant enforces it
    // Renaming a credential directory cannot grant another client's topic rights
    char cert_path[4096], subject[32] = {0};
    int length = snprintf(cert_path, sizeof(cert_path), "%s/identity.pem", credentials);
    if (length < 0 || (size_t)length >= sizeof(cert_path))
        return false;
    FILE *file = fopen(cert_path, "r");
    X509 *cert = file ? PEM_read_X509(file, NULL, NULL, NULL) : NULL;
    if (file) fclose(file);
    if (!cert) return false;
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName,
                             subject, sizeof(subject));
    X509_free(cert);
    if (server ? strcmp(subject, "server") != 0 :
        (strlen(subject) != 7 || strncmp(subject, "client", 6) != 0 ||
         subject[6] < '1' || subject[6] > '6'))
        return false;
    net->identity = server ? 0 : subject[6] - '1';

    // WSL/LAN deployments need predictable Cyclone DDS discovery ports so a
    // narrowly scoped UDP firewall rule can allow the game. Auto keeps
    // multiple local processes (server + several clients) on distinct ports.
    // Respect an explicit operator configuration if one was supplied.
    if (!getenv("CYCLONEDDS_URI"))
    {
        static const char dds_config[] =
            "<CycloneDDS><Domain><Discovery>"
            "<ParticipantIndex>auto</ParticipantIndex>"
            "<MaxAutoParticipantIndex>15</MaxAutoParticipantIndex>"
            "</Discovery></Domain></CycloneDDS>";
        if (setenv("CYCLONEDDS_URI", dds_config, 0) != 0)
        {
            fprintf(stderr, "Could not set Cyclone DDS discovery configuration: %s\n",
                    strerror(errno));
            return false;
        }
    }

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
    for (int i = 0; i < NET_IDENTITIES; ++i)
    {
        if (!server && i != net->identity) continue;
        char name[48];
        snprintf(name, sizeof(name), "JoinRequest_client%d", i + 1);
        net->join_readers[i] = endpoint(net, &JoinRequest_desc, name, !server, true, 8);
        snprintf(name, sizeof(name), "JoinResponse_client%d", i + 1);
        net->replies[i] = endpoint(net, &JoinResponse_desc, name, server, true, 8);
        snprintf(name, sizeof(name), "GameState_client%d", i + 1);
        net->states[i] = endpoint(net, &GameState_desc, name, server, false, 1);
        if (net->join_readers[i] < 0 || net->replies[i] < 0 || net->states[i] < 0)
            goto fail;
    }
    net->join = net->join_readers[net->identity];
    net->response = net->replies[net->identity];
    net->state = net->states[net->identity];
    net->input = endpoint(net, &Input_desc, "Input", !server, true, 32);
    if (net->input < 0) goto fail;

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

dds_return_t net_server_take_join(Network *net, void **samples,
    dds_sample_info_t *info, size_t size, uint32_t count)
{
    // Round-robin queues prevent 1 signed identity monopolising all join reads
    for (unsigned n = 0; n < NET_IDENTITIES; ++n)
    {
        unsigned i = net->next_reader++ % NET_IDENTITIES;
        net->join = net->join_readers[i];
        dds_return_t result = dds_take(net->join, samples, info, size, count);
        if (result != 0) return result;
    }
    return 0;
}

void net_server_retire(Network *net, const GameState *state)
{
    // Disposal messages contain room keys too; send them only on channels that
    // belonged to this room, never to every credential on the server
    for (int i = 0; i < NET_PLAYERS; ++i)
        if (net->sessions[i].session_id != 0)
            checked(dds_unregister_instance(net->states[net->sessions[i].identity], state),
                    "Retire private room state");
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
                               .session_id = token, .client_token = net->pending_token,
                               .identity = net->pending_identity, .last_seen = net_now()};
    }

    if (player_id > 0)
    {
        NetAdmission *admission = &net->admissions[net->pending_identity];
        admission->owner = net->pending_id;
        admission->room_code = net->pending_room;
        admission->expires = net_now() + NET_TIMEOUT_NS;
    }
    JoinResponse response = {.int_player_id = player_id,
        .room_code = net->pending_room, .client_token = net->pending_token};
    if (player_id > 0)
        response.session_id = net->sessions[player_id - 1].session_id;
    memcpy(response.client_id, net->pending_id.v, sizeof(response.client_id));
    return checked(dds_write(net->replies[net->pending_identity], &response), "Send join reply");
}

NetEvent net_server_expire(Network *net)
{
    int64_t now = net_now();
    for (int i = 0; i < NET_PLAYERS; ++i)
    {
        if (net->sessions[i].active && now - net->sessions[i].last_seen >= NET_TIMEOUT_NS)
        {
            GameState retired = {.room_code = net->room_code};
            checked(dds_unregister_instance(net->states[net->sessions[i].identity], &retired),
                    "Expire private room state");
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
    if (!info->valid_data || !net_valid_name(request->display_name) || !sender(net->join, info, &owner) ||
        memcmp(request->client_id, owner.v, sizeof(request->client_id)) != 0 ||
        (request->room_code != 0 && (request->room_code < 100000 || request->room_code > 999999)) ||
        (net->room_code != 0 && request->room_code != net->room_code))
    {
        ++net->rejected;
        return false;
    }

    // Topic permission binds the request to a certificate identity; never use
    // a claimed player ID or a freely chosen participant GUID as the quota key
    dds_builtintopic_endpoint_t *publication =
        dds_get_matched_publication_data(net->join, info->publication_handle);
    if (!publication) return false;
    int identity = -1;
    for (int i = 0; i < NET_IDENTITIES; ++i)
    {
        char topic[48];
        snprintf(topic, sizeof(topic), "JoinRequest_client%d", i + 1);
        if (strcmp(publication->topic_name, topic) == 0) identity = i;
    }
    dds_builtintopic_free_endpoint(publication);
    if (identity < 0) return false;
    NetAdmission *admission = &net->admissions[identity];
    int64_t now = net_now();
    if (net->room_code == 0)
    {
        // 5 attempts per second includes retries and failed room-code guesses
        if (now < admission->next_join)
        {
            ++net->rejected;
            return false;
        }
        admission->next_join = now + INT64_C(200000000);
    }
    if (admission->expires > now &&
        (admission->room_code != request->room_code || !same_id(&admission->owner, &owner)))
    {
        ++net->rejected;
        return false;
    }
    net->pending_identity = identity;
    net->pending_id = owner;
    net->pending_room = request->room_code;
    net->pending_token = request->client_token;
    for (int i = 0; i < NET_PLAYERS; ++i)
    {
        if (net->sessions[i].active && same_id(&owner, &net->sessions[i].owner))
        {
            // A new request token must not inherit an earlier admission
            if (request->client_token != net->sessions[i].client_token)
            {
                ++net->rejected;
                return false;
            }
            JoinResponse response = {.int_player_id = i + 1,
                .session_id = net->sessions[i].session_id,
                .room_code = request->room_code, .client_token = request->client_token};
            memcpy(response.client_id, owner.v, sizeof(response.client_id));
            checked(dds_write(net->replies[identity], &response), "Resend join reply");
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
        strchr("lrncq", input->ch_direction) != NULL &&
        input->room_code == net->room_code &&
        (!input->leave_room || input->ch_direction == 'q');
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
    net->admissions[session->identity].expires = now + NET_TIMEOUT_NS;
    if (input->ch_direction == 'q')
    {
        GameState retired = {.room_code = net->room_code};
        checked(dds_unregister_instance(net->states[session->identity], &retired),
                "Leave private room state");
        session->active = false;
        net->admissions[session->identity].expires = 0;
        JoinResponse closed = {.int_player_id = 0, .room_code = net->room_code,
            .client_token = session->client_token, .session_id = session->session_id};
        memcpy(closed.client_id, owner.v, sizeof(closed.client_id));
        checked(dds_write(net->replies[session->identity], &closed), "Confirm leave");
        return (NetEvent){NET_LEAVE, input->int_player_id, 'q'};
    }
    if (input->ch_direction == 'n')
        return (NetEvent){NET_HEARTBEAT, input->int_player_id, 'n'};
    if (input->ch_direction == 'c')
    {
        if (now - session->last_move >= NET_INPUT_NS)
        {
            session->last_move = now;
            return (NetEvent){NET_CONTINUE, input->int_player_id, 'c'};
        }
        ++net->rejected;
        return (NetEvent){0};
    }
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
        if (event.type == NET_MOVE || event.type == NET_LEAVE || event.type == NET_CONTINUE)
            return event;
    }

    JoinRequest request;
    void *samples[] = {&request};
    if (net_server_take_join(net, samples, &info, 1, 1) == 1 &&
        net_server_join(net, &request, &info))
        return (NetEvent){NET_JOIN, 0, 0};
    return (NetEvent){0};
}

bool net_server_state(Network *net, GameState *state)
{
    // Give each complete snapshot a seq so clients can discard old state
    state->seq = ++net->state_sequence;
    bool ok = true;
    for (int i = 0; i < NET_PLAYERS; ++i)
        if (net->sessions[i].active)
            ok = checked(dds_write(net->states[net->sessions[i].identity], state),
                         "Send private game state") && ok;
    return ok;
}

static void expire_client(Network *net, int64_t now)
{
    if (now - net->last_state >= NET_TIMEOUT_NS)
    {
        // Clear the old server after a timeout and start joining again
        net->player_id = -1;
        net->session_id = 0;
        net->server_id = (dds_guid_t){0};
        net->state_sequence = 0;
    }
}

bool net_client_send(Network *net, char direction)
{
    int64_t now = net_now();

    expire_client(net, now);

    if (net->player_id == -1)
    {
        if (now - net->last_join < INT64_C(500000000))
            return true;

        JoinRequest request = {.room_code = net->room_code,
            .create_room = net->create_room, .client_token = net->client_token};
        memcpy(request.display_name, net->display_name, sizeof(request.display_name));
        memcpy(request.client_id, net->own_id.v, sizeof(request.client_id));
        net->last_join = now;
        return checked(dds_write(net->join, &request), "Send join request");
    }

    if (net->player_id == 0)
        return true;

    // Send idle heartbeats every 100 ms and movement no faster than every 10 ms
    int64_t interval = direction == 'n' ? INT64_C(100000000) : NET_INPUT_NS;
    if (direction != 'q' && now - net->last_input < interval)
        return true;

    Input input = {.room_code = net->room_code, .int_player_id = net->player_id,
        .ch_direction = direction, .leave_room = direction == 'q',
        .seq = ++net->seq, .session_id = net->session_id};
    net->last_input = now;
    return checked(dds_write(net->input, &input), "Send input");
}

static bool valid_state(const GameState *state)
{
    // Bounds protect rendering even when an authenticated peer sends bad data
    // Game rules stay in game.c; these checks only describe the transport contract
    if (state->ready_players < 0 || state->ready_players >= (1 << NET_PLAYERS)) return false;
    if (state->contenders < 0 || state->contenders >= (1 << NET_PLAYERS) ||
        state->danger_lane < -1 || state->danger_lane > 3) return false;
    for (int i = 0; i < NET_PLAYERS; ++i)
        if (state->bonus_score[i] < 0) return false;
    if (state->phase < 0 || state->phase > 3 || state->level < 0 || state->level > 4 ||
        state->time_left < 0 ||
        state->time_left > 60 || state->winners < 0 || state->winners >= (1 << NET_PLAYERS) ||
        state->best_score < 0)
        return false;

    for (int i = 0; i < NET_PLAYERS; ++i)
        if (state->player_x[i] < 0 || state->player_x[i] > 750 ||
            state->player_y[i] < 0 || state->player_y[i] > 550 || state->player_score[i] < 0 ||
            state->player_lives[i] < 0 || state->player_lives[i] > 3)
            return false;

    for (int i = 0; i < NET_PLAYERS; ++i)
        if (!net_valid_name(state->player_names[i])) return false;

    // Coins use the game's top-left coordinates; fixed sizes also bound draw work
    for (int i = 0; i < NET_COINS; ++i)
        if (state->coin_x[i] < 0 || state->coin_x[i] > 800 ||
            state->coin_y[i] < -620 || state->coin_y[i] > 620 ||
            (state->coin_active[i] && state->coin_size[i] != 14 && state->coin_size[i] != 20))
            return false;
    for (int i = 0; i < NET_BOMBS; ++i)
        if (state->bomb_x[i] < 0 || state->bomb_x[i] > 800 ||
            state->bomb_y[i] < -700 || state->bomb_y[i] > 620 ||
            (state->bomb_active[i] && state->bomb_size[i] != 24))
            return false;
    return true;
}

void net_client_room(Network *net, int32_t room_code, bool create_room, uint64_t token)
{
    // Clear the previous admission before changing routing; delayed replies must
    // match this request token as well as our authenticated participant GUID
    net->room_code = room_code;
    net->create_room = create_room;
    net->client_token = token;
    net->player_id = -1;
    net->session_id = 0;
    net->server_id = (dds_guid_t){0};
    net->state_sequence = 0;
    net->last_join = 0;
    net->last_state = net_now();
}

bool net_client_state(Network *net, GameState *state)
{
    // An expired admission no longer receives private state; detect that silence
    // even while input sending is paused
    expire_client(net, net_now());
    JoinResponse response;
    dds_sample_info_t info;
    dds_guid_t owner;

    if (take(net->response, &response, &info) && net->player_id == -1 &&
        memcmp(response.client_id, net->own_id.v, sizeof(response.client_id)) == 0 &&
        response.room_code == net->room_code && response.client_token == net->client_token &&
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
    if (!take(net->state, &sample, &info) || net->player_id <= 0)
        return false;

    // Only accept snapshots from the server that assigned this player slot
    if (!sender(net->state, &info, &owner) || !same_id(&owner, &net->server_id) ||
        sample.room_code != net->room_code || sample.seq <= net->state_sequence || !valid_state(&sample))
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
    if (!state->player_active[net->player_id - 1])
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
        Input leave = {.room_code = net->room_code, .int_player_id = net->player_id,
            .ch_direction = 'q', .leave_room = true, .seq = ++net->seq,
            .session_id = net->session_id};
        if (dds_write(net->input, &leave) == DDS_RETCODE_OK)
        {
            // Keep the authenticated writer discoverable until the server processes q.
            int64_t deadline = net_now() + DDS_MSECS(1000);
            while (net_now() < deadline)
            {
                JoinResponse response;
                dds_sample_info_t info;
                dds_guid_t owner;
                if (take(net->response, &response, &info) && response.int_player_id == 0 &&
                    response.room_code == net->room_code && response.client_token == net->client_token &&
                    response.session_id == net->session_id &&
                    memcmp(response.client_id, net->own_id.v, sizeof(response.client_id)) == 0 &&
                    sender(net->response, &info, &owner) && same_id(&owner, &net->server_id))
                    break;
                dds_sleepfor(DDS_MSECS(1));
            }
        }
    }

    dds_delete(net->participant);
    net->participant = 0;
}
