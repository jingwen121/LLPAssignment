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
#include "messages.h"

#define MAX_ROOMS 8
#define MAX_BATCH 16
#define FIRST_ROOM_CODE 100000
#define LAST_ROOM_CODE 999999

typedef struct
{
    bool active;
    int32_t code;
    uint64_t client_tokens[MAX_PLAYERS];
    Game game;
} Room;

static Room g_rooms[MAX_ROOMS];
static pthread_mutex_t g_rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void)
{
    return (uint64_t)(dds_time() / 1000000u);
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

static bool create_dds_entities(dds_entity_t *participant,
                                dds_entity_t *join_reader,
                                dds_entity_t *response_writer,
                                dds_entity_t *input_reader,
                                dds_entity_t *state_writer)
{
    *participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (*participant < 0)
        return false;

    dds_entity_t join_topic = dds_create_topic(*participant, &JoinRequest_desc,
                                                "JoinRequest", NULL, NULL);
    dds_entity_t response_topic = dds_create_topic(*participant, &JoinResponse_desc,
                                                   "JoinResponse", NULL, NULL);
    dds_entity_t input_topic = dds_create_topic(*participant, &Input_desc,
                                                 "Input", NULL, NULL);
    dds_entity_t state_topic = dds_create_topic(*participant, &GameState_desc,
                                                 "GameState", NULL, NULL);
    if (join_topic < 0 || response_topic < 0 || input_topic < 0 || state_topic < 0)
        return false;

    dds_qos_t *qos = dds_create_qos();
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 64);
    *join_reader = dds_create_reader(*participant, join_topic, qos, NULL);
    *input_reader = dds_create_reader(*participant, input_topic, qos, NULL);
    dds_delete_qos(qos);
    *response_writer = dds_create_writer(*participant, response_topic, NULL, NULL);
    *state_writer = dds_create_writer(*participant, state_topic, NULL, NULL);
    return *join_reader >= 0 && *input_reader >= 0 &&
           *response_writer >= 0 && *state_writer >= 0;
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
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        out->player_active[i] = game->players[i].active;
        out->player_x[i] = game->players[i].x;
        out->player_y[i] = game->players[i].y;
        out->player_score[i] = game->players[i].score;
    }
    for (int i = 0; i < MAX_COINS; ++i)
    {
        out->coin_active[i] = game->coins[i].active;
        out->coin_gold[i] = game->coins[i].gold;
        out->coin_x[i] = game->coins[i].x;
        out->coin_y[i] = game->coins[i].y;
        out->coin_size[i] = game->coins[i].size;
    }
}

static void *dds_worker(void *unused)
{
    (void)unused;
    dds_entity_t participant = 0, join_reader = 0, response_writer = 0;
    dds_entity_t input_reader = 0, state_writer = 0;
    if (!create_dds_entities(&participant, &join_reader, &response_writer,
                             &input_reader, &state_writer))
    {
        fprintf(stderr, "Server DDS setup failed.\n");
        if (participant > 0)
            dds_delete(participant);
        return NULL;
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
        dds_return_t count = dds_take(join_reader, join_samples, join_info,
                                      MAX_BATCH, MAX_BATCH);
        if (count < 0)
            fprintf(stderr, "Join read failed: %s\n", dds_strretcode(-count));
        for (dds_return_t n = 0; n < count; ++n)
        {
            if (!join_info[n].valid_data)
                continue;
            JoinRequest *request = (JoinRequest *)join_samples[n];
            JoinResponse response = {0};
            response.client_token = request->client_token;
            response.room_code = request->room_code;

            int room_index = -1;
            pthread_mutex_lock(&g_rooms_mutex);
            int existing = find_room(request->room_code);
            if (request->create_room)
            {
                if (existing < 0 && request->room_code >= FIRST_ROOM_CODE &&
                    request->room_code <= LAST_ROOM_CODE)
                {
                    room_index = create_room(request->room_code);
                    if (room_index >= 0)
                        last_sent_tick[room_index] = UINT32_MAX;
                }
                else if (existing >= 0 && find_token(&g_rooms[existing], request->client_token))
                    room_index = existing; /* safe retry of the same create request */
            }
            else
                room_index = existing;

            if (room_index >= 0)
            {
                Room *room = &g_rooms[room_index];
                response.int_player_id = find_token(room, request->client_token);
                if (response.int_player_id == 0)
                {
                    response.int_player_id = game_add_player(&room->game);
                    if (response.int_player_id > 0)
                        room->client_tokens[response.int_player_id - 1] = request->client_token;
                }
            }
            pthread_mutex_unlock(&g_rooms_mutex);

            if (response.int_player_id > 0)
                printf("Player %d joined room %06d\n", (int)response.int_player_id,
                       (int)response.room_code);
            dds_return_t rc = dds_write(response_writer, &response);
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
                if (input->leave_room)
                {
                    int slot = input->int_player_id - 1;
                    room->client_tokens[slot] = 0;
                    game_remove_player(&room->game, input->int_player_id);
                    if (game_num_active(&room->game) == 0)
                    {
                        room->active = false;
                        last_sent_tick[room_index] = UINT32_MAX;
                    }
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
            dds_return_t rc = dds_write(state_writer, &state);
            if (rc < 0)
                fprintf(stderr, "State publish failed: %s\n", dds_strretcode(-rc));
        }
        dds_sleepfor(DDS_MSECS(3));
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned int)time(NULL));
    pthread_t worker;
    if (pthread_create(&worker, NULL, dds_worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
    printf("Coin Rush room server ready (up to %d rooms, %d players each).\n",
           MAX_ROOMS, MAX_PLAYERS);

    uint64_t last_status = 0;
    for (;;)
    {
        uint64_t now = now_ms();
        pthread_mutex_lock(&g_rooms_mutex);
        for (int i = 0; i < MAX_ROOMS; ++i)
            if (g_rooms[i].active)
                game_tick(&g_rooms[i].game, now);
        if (now - last_status >= 1000)
        {
            last_status = now;
            for (int i = 0; i < MAX_ROOMS; ++i)
                if (g_rooms[i].active)
                    printf("Room %06d: %d player(s), phase %d, %ds left\n",
                           (int)g_rooms[i].code, game_num_active(&g_rooms[i].game),
                           (int)g_rooms[i].game.phase, g_rooms[i].game.time_left);
        }
        pthread_mutex_unlock(&g_rooms_mutex);
        dds_sleepfor(DDS_MSECS(TICK_MS));
    }
    return 0;
}
