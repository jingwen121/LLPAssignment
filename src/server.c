/* Coin Rush room server: authoritative simulation and room-code matchmaking. */
#include <dds/dds.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include <messages.h>
#include "network.h"

#define MAX_ROOMS 8
#define MAX_BATCH 16
#define FIRST_ROOM_CODE 100001
#define LAST_ROOM_CODE 999999

_Static_assert(MAX_ROOMS == NET_ROOMS, "Room capacities must match");
_Static_assert(MAX_PLAYERS == NET_PLAYERS, "Player capacities must match");
_Static_assert(MAX_COINS == NET_COINS, "Coin capacities must match");

typedef struct
{
    bool active;
    int32_t code;
    uint64_t client_tokens[MAX_PLAYERS];
    Game game;

    // Each room owns its player sessions; only DDS handles are shared
    Network network;
} Room;

static Room g_rooms[MAX_ROOMS];
static pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void)
{
    return (uint64_t)(net_now() / INT64_C(1000000));
}

static int find_room(int32_t code)
{
    for (int i = 0; i < MAX_ROOMS; ++i)
        if (g_rooms[i].active && g_rooms[i].code == code)
            return i;
    return -1;
}

static int find_token(const Room *room, uint64_t token)
{
    if (token == 0)
        return 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (room->client_tokens[i] == token)
            return i + 1;
    return 0;
}

static int create_room(int32_t code)
{
    for (int i = 0; i < MAX_ROOMS; ++i)
        if (!g_rooms[i].active)
        {
            memset(&g_rooms[i], 0, sizeof(g_rooms[i]));
            g_rooms[i].active = true;
            g_rooms[i].code = code;
            game_init(&g_rooms[i].game);
            return i;
        }
    return -1;
}

static bool create_dds_entities(Network *net, const char *credentials,
                                dds_entity_t *participant,
                                dds_entity_t *join_reader,
                                dds_entity_t *response_writer,
                                dds_entity_t *input_reader,
                                dds_entity_t *state_writer)
{
    if (!net_open(net, true, credentials))
        return false;
    *participant = net->participant;
    *join_reader = net->join;
    *response_writer = net->response;
    *input_reader = net->input;
    *state_writer = net->state;
    return true;
}

static void fill_game_state(GameState *out, const Room *room)
{
    const Game *game = &room->game;
    memset(out, 0, sizeof(*out));
    out->room_code = room->code;
    out->tick = game->tick;
    out->phase = game->phase;
    out->time_left = game->time_left;
    out->winners = game->winners;
    out->best_score = game->best_score;
    out->level = game->level;
    out->ready_players = game->ready_players;
    out->contenders = game->contenders;
    out->danger_lane = game->danger_lane;
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        memcpy(out->player_names[i], game->players[i].name, sizeof(out->player_names[i]));
        out->player_active[i] = game->players[i].active;
        out->player_x[i] = game->players[i].x;
        out->player_y[i] = game->players[i].y;
        out->player_score[i] = game->players[i].score;
        out->bonus_score[i] = game->bonus_score[i];
        out->player_lives[i] = game->players[i].lives;
        out->player_collect_seq[i] = game->players[i].collect_seq;
        out->player_bomb_seq[i] = game->players[i].bomb_seq;
    }
    for (int i = 0; i < MAX_COINS; ++i)
    {
        out->coin_active[i] = game->coins[i].active;
        out->coin_gold[i] = game->coins[i].gold;
        out->coin_x[i] = game->coins[i].x;
        out->coin_y[i] = game->coins[i].y;
        out->coin_size[i] = game->coins[i].size;
    }
    for (int i = 0; i < MAX_BOMBS; ++i)
    {
        out->bomb_active[i] = game->bombs[i].active;
        out->bomb_x[i] = game->bombs[i].x;
        out->bomb_y[i] = game->bombs[i].y;
        out->bomb_size[i] = game->bombs[i].size;
    }
}

static void *dds_worker(void *credentials)
{
    Network net;
    dds_entity_t participant = 0, join_reader = 0, response_writer = 0;
    dds_entity_t input_reader = 0, state_writer = 0;
    if (!create_dds_entities(&net, credentials, &participant, &join_reader, &response_writer,
                             &input_reader, &state_writer))
    {
        fprintf(stderr, "Server DDS setup failed.\n");
        if (participant > 0)
            dds_delete(participant);
        exit(EXIT_FAILURE);
    }

    void *join_samples[MAX_BATCH] = {0};
    void *input_samples[MAX_BATCH] = {0};
    dds_sample_info_t join_info[MAX_BATCH];
    dds_sample_info_t input_info[MAX_BATCH];
    for (int i = 0; i < MAX_BATCH; ++i)
    {
        join_samples[i] = JoinRequest__alloc();
        input_samples[i] = Input__alloc();
        if (!join_samples[i] || !input_samples[i])
        {
            fprintf(stderr, "Server could not allocate DDS samples.\n");
            return NULL;
        }
    }

    uint32_t last_sent_tick[MAX_ROOMS];
    for (int i = 0; i < MAX_ROOMS; ++i)
        last_sent_tick[i] = UINT32_MAX;

    for (;;)
    {
        dds_return_t count = net_server_take_join(&net, join_samples, join_info,
                                      MAX_BATCH, MAX_BATCH);
        if (count < 0)
            fprintf(stderr, "Join read failed: %s\n", dds_strretcode(-count));
        for (dds_return_t n = 0; n < count; ++n)
        {
            if (!join_info[n].valid_data)
                continue;
            JoinRequest *request = (JoinRequest *)join_samples[n];
            // Validate the authenticated sender before room creation or slot allocation
            if (!net_server_join(&net, request, &join_info[n]))
                continue;
            JoinResponse response = {0};
            response.client_token = request->client_token;
            response.room_code = request->room_code;

            int room_index = -1;
            pthread_mutex_lock(&g_rooms_mutex);
            int existing = find_room(request->room_code);
            if (request->create_room)
            {
                if (existing < 0 &&
                    ((request->room_code == NET_QUICKPLAY_ROOM_CODE) ||
                     (request->room_code >= FIRST_ROOM_CODE &&
                      request->room_code <= LAST_ROOM_CODE)))
                {
                    room_index = create_room(request->room_code);
                    if (room_index >= 0)
                    {
                        last_sent_tick[room_index] = UINT32_MAX;
                        g_rooms[room_index].network = net;
                        g_rooms[room_index].network.room_code = request->room_code;
                    }
                }
                else if (existing >= 0 &&
                         request->room_code == NET_QUICKPLAY_ROOM_CODE)
                {
                    /* Every authenticated quick-play client uses the same
                     * reserved room. The first request creates it; later
                     * clients join it without sharing a room-code prompt. */
                    room_index = existing;
                }
                else if (existing >= 0 && find_token(&g_rooms[existing], request->client_token))
                    room_index = existing; /* safe retry of the same create request */
            }
            else
                room_index = existing;

            if (room_index >= 0)
            {
                Room *room = &g_rooms[room_index];

                // Retry replies come from the room's GUID-bound session; a matching
                // client token alone must never recover somebody else's slot
                room->network.join = net.join;
                if (!net_server_join(&room->network, request, &join_info[n]))
                {
                    pthread_mutex_unlock(&g_rooms_mutex);
                    continue;
                }
                response.int_player_id = find_token(room, request->client_token);
                if (response.int_player_id == 0)
                {
                    response.int_player_id = game_add_player(&room->game);
                    if (response.int_player_id > 0) {
                        room->client_tokens[response.int_player_id - 1] = request->client_token;
                        if (request->display_name[0])
                            memcpy(room->game.players[response.int_player_id - 1].name,
                                   request->display_name, sizeof(request->display_name));
                        printf("%s joined (%d active)\n", room->game.players[response.int_player_id - 1].name, game_num_active(&room->game));
                    }
                }
            }
            Network *reply = room_index >= 0 ? &g_rooms[room_index].network : &net;
            pthread_mutex_unlock(&g_rooms_mutex);

            if (response.int_player_id > 0)
                printf("%s joined room %06d\n", request->display_name[0] ? request->display_name : "Unnamed player",
                       (int)response.room_code);
            // Keep token generation and the encrypted reply in the shared helper
            dds_return_t rc = net_server_accept(reply, response.int_player_id) ?
                DDS_RETCODE_OK : DDS_RETCODE_ERROR;
            if (rc < 0)
                fprintf(stderr, "Join reply failed: %s\n", dds_strretcode(-rc));
        }

        count = dds_take(input_reader, input_samples, input_info, MAX_BATCH, MAX_BATCH);
        if (count < 0)
            fprintf(stderr, "Input read failed: %s\n", dds_strretcode(-count));
        for (dds_return_t n = 0; n < count; ++n)
        {
            if (!input_info[n].valid_data)
                continue;
            Input *input = (Input *)input_samples[n];
            pthread_mutex_lock(&g_rooms_mutex);
            int room_index = find_room(input->room_code);
            if (room_index >= 0 && input->int_player_id >= 1 &&
                input->int_player_id <= MAX_PLAYERS)
            {
                Room *room = &g_rooms[room_index];
                NetEvent event = net_server_input(&room->network, input, &input_info[n]);
                if (event.type == NET_NONE)
                {
                    pthread_mutex_unlock(&g_rooms_mutex);
                    continue;
                }
                if (event.type == NET_LEAVE)
                {
                    int slot = input->int_player_id - 1;
                    room->client_tokens[slot] = 0;
                    game_remove_player(&room->game, input->int_player_id);
                    if (game_num_active(&room->game) == 0)
                    {
                        // Release the keyed DDS instance before reusing this room slot
                        GameState empty = {.room_code = room->code};
                        net_server_retire(&room->network, &empty);
                        room->active = false;
                        last_sent_tick[room_index] = UINT32_MAX;
                    }
                }
                else if (event.type == NET_CONTINUE)
                {
                    game_continue_level(&room->game, input->int_player_id, now_ms());
                }
                else
                {
                    int direction = input->ch_direction == 'l' ? -1 :
                                    input->ch_direction == 'r' ? 1 : 0;
                    game_set_input(&room->game, input->int_player_id, direction, now_ms());
                }
            }
            pthread_mutex_unlock(&g_rooms_mutex);
        }

        // A crashed client cannot send leave; expire its session and release the
        // same game slot that an orderly leave would release
        pthread_mutex_lock(&g_rooms_mutex);
        for (int i = 0; i < MAX_ROOMS; ++i)
        {
            if (!g_rooms[i].active)
                continue;
            NetEvent event;
            while ((event = net_server_expire(&g_rooms[i].network)).type == NET_LEAVE)
            {
                g_rooms[i].client_tokens[event.player_id - 1] = 0;
                game_remove_player(&g_rooms[i].game, event.player_id);
            }
            if (game_num_active(&g_rooms[i].game) == 0)
            {
                GameState empty = {.room_code = g_rooms[i].code};
                net_server_retire(&g_rooms[i].network, &empty);
                g_rooms[i].active = false;
                last_sent_tick[i] = UINT32_MAX;
            }
        }
        pthread_mutex_unlock(&g_rooms_mutex);

        Room snapshot[MAX_ROOMS];
        pthread_mutex_lock(&g_rooms_mutex);
        memcpy(snapshot, g_rooms, sizeof(snapshot));
        pthread_mutex_unlock(&g_rooms_mutex);
        for (int i = 0; i < MAX_ROOMS; ++i)
        {
            if (!snapshot[i].active || snapshot[i].game.tick == last_sent_tick[i])
                continue;
            last_sent_tick[i] = snapshot[i].game.tick;
            GameState state;
            fill_game_state(&state, &snapshot[i]);
            // Keep sequences local to each room so other rooms do not look like loss
            dds_return_t rc = net_server_state(&g_rooms[i].network, &state) ?
                DDS_RETCODE_OK : DDS_RETCODE_ERROR;
            if (rc < 0)
                fprintf(stderr, "State publish failed: %s\n", dds_strretcode(-rc));
        }
        dds_sleepfor(DDS_MSECS(3));
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *credentials = argc > 1 ? argv[1] : "credentials-private/server";
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned int)time(NULL));
    pthread_t worker;
    if (pthread_create(&worker, NULL, dds_worker, (void *)credentials) != 0)
    {
        perror("pthread_create");
        return 1;
    }
    printf("Coin Rush room server ready (up to %d rooms, %d players each).\n",
           MAX_ROOMS, MAX_PLAYERS);

    for (;;)
    {
        uint64_t now = now_ms();
        pthread_mutex_lock(&g_rooms_mutex);
        for (int i = 0; i < MAX_ROOMS; ++i)
            if (g_rooms[i].active)
                game_tick(&g_rooms[i].game, now);
        pthread_mutex_unlock(&g_rooms_mutex);
        dds_sleepfor(DDS_MSECS(TICK_MS));
    }
    return 0;
}
