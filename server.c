// Build with make and run ./build/server credentials/server

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS server
2. Supports max 5 clients
3. Start this server before starting the clients
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported */

/* mutex lock/unlock must be matched
watch out for break out of loop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>
#include "network.h"

#include "dds/dds.h"
#include "messages.h"

// ### GLOBAL CONSTANTS ###
#define MAX_SAMPLES 1
#define MAX_PLAYERS 5

#define WIDTH  800
#define HEIGHT 600
#define SQ_WIDTH 50
// How many pixels is 1 step
#define STEP 1

// Player coord defined as NW corner of square
typedef struct
{
    bool b_active;
    int x;
    int y;
    int score;
} Player;

// ### GLOBAL VARIABLES ###
int int_num_active_players = 0;

// Flag set by worker thread that a player just joined
// Must be reset to false after processing by main thread
bool b_player_join = false;

// Player ID is the array index plus 1
// -1 = No new players
// 0 = Invalid player ID (unable to admit new players)
// 1~MAX_PLAYERS = Valid player IDs
int int_new_player_id = -1;

// Table to track clients
// Init all members to 0
static Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// The main thread owns slot allocation; A condition variable lets the worker
// sleep while waiting for that decision; it releases mutex during the wait
static pthread_cond_t join_ready = PTHREAD_COND_INITIALIZER;

// These flags cross thread boundaries outside mutex; A failed secure startup
// must stop main as well, otherwise the program would appear to run without DDS
static atomic_bool running = true;
static atomic_int worker_status = 0;

// Update round fields and coins under mutex; networking
// copies this state but never invents coins, scores, round timing or a winner
static GameState round_state;

static void stop_server(int signal_number)
{
    (void)signal_number;
    atomic_store(&running, false);
}


// Check if the coords collide with other squares
bool detect_collision(Player a_arr_players[], int a_int_arr_len, 
						int a_int_id, int a_int_x, int a_int_y) 
{
	bool b_res = false;
	// Loop through the array
	for(int i = 0; i < a_int_arr_len; i++)
	{
		// Index + 1 = Player ID
		// If a player other than the player itself is active
		if(i != a_int_id -1 && a_arr_players[i].b_active == true)
		{
			// If they overlap horizontally
			if(a_int_x <= a_arr_players[i].x + SQ_WIDTH && 
				a_arr_players[i].x <= a_int_x + SQ_WIDTH)
			{
				// If they overlap vertically
				if(a_int_y <= a_arr_players[i].y + SQ_WIDTH && 
					a_arr_players[i].y <= a_int_y + SQ_WIDTH)
				{
					b_res = true;
					break;
				}
			}
		}
	}
	
	return b_res;
}

void *legacy_worker(void *arg)
{

    // This worker does not use the argument passed by pthread_create
    (void)arg;

	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t position_topic;
	dds_entity_t input_topic;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t position_writer;
	dds_entity_t input_reader;
		
	//Pointer to unknown type
	void *samples_join[MAX_SAMPLES];	
	dds_sample_info_t infos_join[MAX_SAMPLES];
	void *samples_input[MAX_SAMPLES];	
	dds_sample_info_t infos_input[MAX_SAMPLES];
	
	dds_return_t rc;
	
	/* ----------------------------------------------------- */
    /* Create participant                                    */
    /* ----------------------------------------------------- */

    participant = dds_create_participant
	(
        DDS_DOMAIN_DEFAULT,
        NULL,
        NULL
    );

    if (participant < 0)
    {
        printf("Failed to create participant\n");

        // Stop the program if the DDS participant could not be created
        exit(EXIT_FAILURE);
    }

    /* ----------------------------------------------------- */
    /* Create topics                                          */
    /* ----------------------------------------------------- */

    join_topic = dds_create_topic
	(
        participant,
        &JoinRequest_desc,
        "JoinRequest",
        NULL,
        NULL
    );
	
	response_topic = dds_create_topic
	(
        participant,
        &JoinResponse_desc,
        "JoinResponse",
        NULL,
        NULL
    );
	
	position_topic = dds_create_topic
	(
        participant,
        &Position_desc,
        "Position",
        NULL,
        NULL
    );
	
	input_topic = dds_create_topic
	(
        participant,
        &Input_desc,
        "Input",
        NULL,
        NULL
    );
	/* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    join_reader = dds_create_reader(
        participant,
        join_topic,
        NULL,
        NULL
    );
	
	input_reader = dds_create_reader(
        participant,
        input_topic,
        NULL,
        NULL
    );
	
	 /* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    response_writer = dds_create_writer(
        participant,
        response_topic,
        NULL,
        NULL
    );
	
	position_writer = dds_create_writer(
        participant,
        position_topic,
        NULL,
        NULL
    );
	
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	samples_input[0] = Input__alloc();
	
	// Create a Join Response
	JoinResponse response;
	
	// Create a Position
	Position position;
	
	// Record the time now
	uint64_t last_time = dds_time();	
	/* Poll until data has been read. */
	while (1)
	{

        // Wait 1 ms before checking again so this loop does not keep the CPU busy
        dds_sleepfor(DDS_MSECS(1));

		// Process a join request
		rc = dds_take(join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check for valid join request
		if ((rc > 0) && (infos_join[0].valid_data))
		{
			printf("Join request received\n");
			// Signal the main thread to check if 
			// number of active players reached
			pthread_mutex_lock(&mutex);
			b_player_join = true;
			pthread_mutex_unlock(&mutex);
		}
		
		// If a new player ID has been assigned by the main thread
		pthread_mutex_lock(&mutex);}
}

void *worker(void *arg)
{
    Network net;

    // Use the server certificate directory passed in by main
    if (!net_open(&net, true, arg))
    {
        atomic_store(&worker_status, 1);
        atomic_store(&running, false);
        return NULL;
    }

    int64_t last_state = 0;
    while (atomic_load(&running))
    {
        // Limit work per pass so joins and movement cannot indefinitely postpone
        // the next snapshot; heartbeats may return NET_NONE after being consumed;
        // another pass follows after the short sleep below
        for (int count = 0; count < 32; ++count)
        {
            NetEvent event = net_server_poll(&net);
            if (event.type == NET_NONE)
                break;

            if (event.type == NET_JOIN)
            {
                // Ask the main thread to choose a player slot and spawn position
                pthread_mutex_lock(&mutex);
                int_new_player_id = -1;
                b_player_join = true;
	
                // Recheck the condition after waking; condition variables may
                // wake without a reply; checking running also allows shutdown
                while (int_new_player_id == -1 && atomic_load(&running))
                    pthread_cond_wait(&join_ready, &mutex);
	
                int assigned_id = int_new_player_id;
                pthread_mutex_unlock(&mutex);
	
                if (assigned_id >= 0 && !net_server_accept(&net, assigned_id))
                {
                    atomic_store(&worker_status, 1);
                    atomic_store(&running, false);
                }
            }
            else if (event.type == NET_LEAVE)
            {
                // Clear all fields so a new owner never inherits another score
                // The game uses the active count to apply its mid-round leave rule;
                // this connection handler deliberately does not end the round
                pthread_mutex_lock(&mutex);
                if (arr_Players[event.player_id - 1].b_active)
                {
                    arr_Players[event.player_id - 1] = (Player){0};
                    --int_num_active_players;
                }
                pthread_mutex_unlock(&mutex);
            }
            else if (event.type == NET_MOVE)
            {
                // The networking code has already checked the sender and input rate
                pthread_mutex_lock(&mutex);
                Player *player = &arr_Players[event.player_id - 1];
                int x = player->x;
                int y = player->y;

                switch (event.direction)
                {
                    case 'l': x -= STEP; break;
                    case 'r': x += STEP; break;
                }
	
                // Keep the existing server movement and collision checks
                if (player->b_active && x >= 0 && x <= WIDTH - SQ_WIDTH &&
                    y >= 0 && y <= HEIGHT - SQ_WIDTH &&
                    !detect_collision(arr_Players, MAX_PLAYERS, event.player_id, x, y))
                {
                    player->x = x;
                    player->y = y;
                }
                pthread_mutex_unlock(&mutex);
            }
        }
	
        // Copy players and round fields in 1 critical section; mixing 2
        // different ticks could show a score with the wrong player positions
        // DDS writes happen after unlocking because reliable operations can wait
        int64_t now = net_now();
        if (now - last_state >= INT64_C(50000000))
        {
            GameState snapshot;
            pthread_mutex_lock(&mutex);
            snapshot = round_state;
            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                snapshot.players[i] = (PlayerState){arr_Players[i].b_active,
                    arr_Players[i].x, arr_Players[i].y, arr_Players[i].score};
            }
            pthread_mutex_unlock(&mutex);

            if (!net_server_state(&net, &snapshot))
            {
                atomic_store(&worker_status, 1);
                atomic_store(&running, false);
            }
			
            last_state = now;
        }
			
        // A 1 ms polling interval bounds idle work without tying networking to
        // rendering; snapshots have their own 50 ms schedule, which is 20 Hz
        dds_sleepfor(DDS_MSECS(1));
    }
						
    net_close(&net);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *credentials = argc > 1 ? argv[1] : "credentials/server";
    srand((unsigned int)time(NULL));
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);
	
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, (void *)credentials) != 0)
    {
        perror("pthread_create");
        return 1;
    }
	
    printf("Waiting for players...\n");
    while (atomic_load(&running))
    {
        pthread_mutex_lock(&mutex);
        if (b_player_join)
        {
            // Return 0 if all player slots are already active
            b_player_join = false;
            int_new_player_id = 0;

            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (arr_Players[i].b_active)
                    continue;

                // Keep the baseline spawn calculation in the game thread
                int x = 0;
                int y = 0;
                bool found = false;
                for (int attempt = 0; attempt < 1000; ++attempt)
                {
                    x = rand() % (WIDTH - SQ_WIDTH + 1);
                    y = rand() % (HEIGHT - SQ_WIDTH + 1);
                    if (!detect_collision(arr_Players, MAX_PLAYERS, i + 1, x, y))
                    {
                        found = true;
                        break;
                    }
                }

                if (!found)
                    break;

                // Set this player to active and save the assigned player ID
                arr_Players[i] = (Player){true, x, y, 0};
                ++int_num_active_players;
                int_new_player_id = i + 1;
                break;
            }

            // Wake the worker now that the player slot has been assigned
            pthread_cond_signal(&join_ready);
        }
        pthread_mutex_unlock(&mutex);
        dds_sleepfor(DDS_MSECS(1));
    }

    // Wake the worker if shutdown happened while it was waiting for a slot
    // Joining before returning keeps its stack arguments and shared state alive
    pthread_mutex_lock(&mutex);
    pthread_cond_broadcast(&join_ready);
    pthread_mutex_unlock(&mutex);
    pthread_join(thread, NULL);
    return atomic_load(&worker_status);
}
