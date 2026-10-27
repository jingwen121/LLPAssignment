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

bool net_open(Network *net, bool server, const char *credentials)
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

    // Joins and input use reliable delivery with a finite writer blocking time
    // Snapshots use best effort and depth 1; receiving a fresh complete state is
    // more useful than retransmitting positions that have already been replaced
    net->join = endpoint(net, &JoinRequest_desc, "JoinRequest", !server, true, 32);
    net->response = endpoint(net, &JoinResponse_desc, "JoinResponse", server, true, 32);
    net->input = endpoint(net, &Input_desc, "Input", !server, true, 32);
    net->state = endpoint(net, &GameState_desc, "GameState", server, false, 1);

    if (net->join < 0 || net->response < 0 || net->input < 0 || net->state < 0)
        goto fail;

    net->last_state = net_now();
    return true;

fail:
    net_close(net);
    return false;
}

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
