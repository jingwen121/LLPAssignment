/* gcc -o server server.c messages.c $(pkg-config --cflags --libs CycloneDDS) */

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS server
2. Supports max 5 clients
3. Start this server, then start client one by one
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

#include "dds/dds.h"
#include "messages.h"
#include "game.h"

// ### GLOBAL CONSTANTS ###
/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1
//#define MAX_PLAYERS 5
//
//#define WIDTH  800
//#define HEIGHT 600
//#define SQ_WIDTH 50
//// How many pixels is one step
//#define STEP 1

// Player coord defined as NW corner of square
//typedef struct
//{
//    bool b_active;
//    int x;
//    int y;
//} Player;

/*
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
*/
// Table to track clients
static Game g_game;
static uint64_t g_client_tokens[MAX_PLAYERS];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Current time in milliseconds, used by the game logic:
//the 60 - second round timer to cehck if the round ended yet 
//the input timeout : if no input arrived in the last 60 ms, the key was released, so the player stops.
static uint64_t now_ms(void)
{
	return (uint64_t)dds_time() / 1000000u;
}

// Check if the coords collide with other squares
/*
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
*/

void *worker(void *arg)
{
	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t state_topic;
	dds_entity_t input_topic;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t state_writer;
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
        //return 1;
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
	
	state_topic = dds_create_topic
	(
        participant,
        &GameState_desc,
        "GameState",
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

	// QoS = "Quality of Service": settings that control how DDS delivers
	// messages. The baseline passed NULL, which means "use the defaults".
	//
	// We change one setting: history (how many unread messages to keep).
	// Default = keep only the LATEST 1. Like a mailbox that holds one
	// letter: if a second message arrives before we read the first, the
	// first is thrown away. With several players that means:
	//   - two clients joining at the same time -> one never gets an ID
	//   - two players holding keys -> one player's input is lost
	// KEEP_LAST 32 lets up to 32 messages wait, so none are lost.
	//
	// the setting belongs to the READER (the mailbox where
	// incoming messages wait), so it is given when the readers are created.
	dds_qos_t* qos = dds_create_qos();  // empty settings object
	dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 32);  // keep last 32 messages

	join_reader = dds_create_reader(
		participant,
		join_topic,
		qos,
		NULL
	);

	input_reader = dds_create_reader(
		participant,
		input_topic,
		qos,
		NULL
    );
	dds_delete_qos(qos); // The readers copied the settings, so the settings object can be freed

	 /* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    response_writer = dds_create_writer(
        participant,
        response_topic,
        NULL,
        NULL
    );
	
	state_writer = dds_create_writer(
        participant,
		state_topic,
        NULL,
        NULL
    );
	
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	samples_input[0] = Input__alloc();
	
	// Create a Join Response
	JoinResponse response;
	
	// One complete snapshot keeps player, coin and HUD values in sync.
	GameState state;
	
	// Last game tick we sent.
	// the game changes 60 times a second; checking the tick number
	// means we send once per change instead of every loop (saves traffic).
	uint32_t last_sent_tick = 0;

	/* Poll until data has been read. */
	while (1)
	{
		// 1. Join requests 
		// Loop until no more requests are waiting.
		// loop here cus the baseline handled only ONE message per pass, so
		// when several arrived they queued up and joins/movement lagged.
		while ((rc = dds_take(join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_join[0].valid_data)
				continue;
			JoinRequest *request = (JoinRequest *)samples_join[0];
			uint64_t token = request->client_token;
			printf("Join request received\n");

			// Ask the game logic for a player ID (0 = game full).
			// Lock only for this one call, so the game loop is never
			// blocked while we do network work.
			pthread_mutex_lock(&mutex);
			int new_id = 0;
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (g_game.players[i].active && g_client_tokens[i] == token)
				{
					new_id = i + 1; /* A retry from this client reuses its slot. */
					break;
				}
			if (new_id == 0)
			{
				new_id = game_add_player(&g_game);
				if (new_id > 0)
					g_client_tokens[new_id - 1] = token;
			}
			pthread_mutex_unlock(&mutex);

			// Send the reply AFTER unlocking.
			// dds_write can take time; never hold the lock during it.
			response.client_token = token;
			response.int_player_id = new_id;
			rc = dds_write(response_writer, &response);
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// 2. Inputs
		while ((rc = dds_take(input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES)) > 0)
		{
			if (!infos_input[0].valid_data)
				continue;
			Input* input = (Input*)samples_input[0];

			// Coin Rush only moves left/right, so convert to -1 / 0 / +1.
			// Up/down ( oranything else) becomes 0 = no movement.
			int dir = 0;
			if (input->ch_direction == 'l') dir = -1;
			if (input->ch_direction == 'r') dir = +1;

			// Only store the direction here; the actual move happens in
			// game_tick(). the server moves everyone at a fixed speed,
			// so sending inputs faster does not make a player faster .
			// game_set_input also checks the player ID is 1..5 and active.
			//  the baseline used the ID directly as an array index, so
			// a fake ID like 99 could write outside the array and crash.
			pthread_mutex_lock(&mutex);
			bool ok = game_set_input(&g_game, input->int_player_id, dir, now_ms());
			pthread_mutex_unlock(&mutex);
			if (!ok)
				printf("Rejected input for player ID %d\n", (int)input->int_player_id);
		}
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		//  3. Send one complete snapshot after each game tick.
		// Copy the game while locked, then send WITHOUT holding the lock.
		// the baseline slept 20 ms per player WHILE locked, so one
		// broadcast to 5 players blocked the other thread for ~100 ms.
		pthread_mutex_lock(&mutex);
		Game snapshot = g_game;
		pthread_mutex_unlock(&mutex);

		// Only send if the game has moved on since the last send.
		//  runs every 2 ms but the game only changes every
		// 16 ms (one tick), so without this check we would send the same
		// positions ~8 times over (wasted network traffic).
		if (snapshot.tick != last_sent_tick)
		{
			// Remember which tick we sent, so we don't send it again
			last_sent_tick = snapshot.tick;

			memset(&state, 0, sizeof(state));
			state.tick = snapshot.tick;
			state.phase = (int32_t)snapshot.phase;
			state.time_left = (int32_t)snapshot.time_left;
			state.winners = (int32_t)snapshot.winners;
			state.best_score = (int32_t)snapshot.best_score;

			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				state.player_active[i] = snapshot.players[i].active;
				state.player_x[i] = snapshot.players[i].x;
				state.player_y[i] = snapshot.players[i].y;
				state.player_score[i] = snapshot.players[i].score;
			}
			for (int i = 0; i < MAX_COINS; i++)
			{
				state.coin_active[i] = snapshot.coins[i].active;
				state.coin_gold[i] = snapshot.coins[i].gold;
				state.coin_x[i] = snapshot.coins[i].x;
				state.coin_y[i] = snapshot.coins[i].y;
				state.coin_size[i] = snapshot.coins[i].size;
			}

			// Publish exactly one sample for this simulation tick.
			rc = dds_write(state_writer, &state);
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));

		}
		//Optimazation
		// Short sleep so this thread doesn't spin at 100% CPU.
		// 2 ms is much shorter than one game tick (16 ms), so new inputs
		// are picked up quickly, but the CPU still gets to rest.
		dds_sleepfor(DDS_MSECS(2));

	}
}

int main(void)
{
	// Print each line immediately.
	// when output is redirected to a log file, C buffers it and the
	// log looks empty until the buffer fills; line buffering avoids that.
	setvbuf(stdout, NULL, _IOLBF, 0);

	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));
	
	// Start with an empty lobby (WAITING state, no players, no coins).
	// Done BEFORE starting the worker, so no join can arrive before the game exists.
	game_init(&g_game);

	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
	
	printf("Coin Rush server running. Waiting for %d players...\n", MIN_PLAYERS);

	/* Main game loop.
	   every player sees the game advance at the same
	   steady rate, no matter how fast each client sends input.
	   I replaced with the original main(), which looped with no sleep at all and
	   used 100% of a CPU core just checking a flag. */
	uint32_t last_print = 0;

	while (1)
	{
		// for(int i = 0; i < MAX_PLAYERS; i++)
		// {
			//printf("%d %d %d\n",i,arr_Players[i].x, arr_Players[i].y);
		// }
		//printf("============\n");
		/* dds_sleepfor (DDS_SECS (0.5)); */
		
		// Run ONE step of the game: players move, coins fall, catches,
		// scores, timer, and state changes (waiting -> playing -> results).
		pthread_mutex_lock(&mutex);
		game_tick(&g_game, now_ms());
		// Copy the state so we can print it after unlocking.
		// Why: printf is slow; don't keep the worker waiting for the lock.
		Game snapshot = g_game;
		pthread_mutex_unlock(&mutex);

		// Print the game status about once per second.
		// Why: printing every tick (60/s) would flood the terminal.
		// 1000 / TICK_MS = number of ticks in one second (~62).
		if (snapshot.tick - last_print >= 1000 / TICK_MS)
		{
			last_print = snapshot.tick;
			game_print(&snapshot);
		}

		// Wait until the next tick (~60 per second) instead of busy-waiting.
		// sleeping lets the CPU rest; the server uses ~1-3% CPU instead of the baseline's 100%.
		dds_sleepfor(DDS_MSECS(TICK_MS));

		//// If a join request has been received
		//pthread_mutex_lock(&mutex);
		//bool b_player_join_temp = b_player_join;
		//pthread_mutex_unlock(&mutex);
		//if (b_player_join_temp == true)
		//{
		//	// printf("b_player_join_temp\n");
		//	// Reset the flag
		//	pthread_mutex_lock(&mutex);
		//	b_player_join = false;
		//	pthread_mutex_unlock(&mutex);
		//	
		//	// If max active players reached
		//	if (int_num_active_players == MAX_PLAYERS)
		//	{
		//		// Set invalid player ID
		//		pthread_mutex_lock(&mutex);
		//		int_new_player_id = 0;
		//		pthread_mutex_unlock(&mutex);
		//	}
		//	else
		//	{
		//		pthread_mutex_lock(&mutex);
		//		// Loop through the players list
		//		for(int i = 0; i < MAX_PLAYERS; i++)
		//		{					
		//			if(arr_Players[i].b_active == false)
		//			{
		//				// Set this index to active
		//				arr_Players[i].b_active = true;
		//				// Increment the number of active players
		//				int_num_active_players++;
		//				
		//				// Calculate initial coords of the new player
		//				int int_rand_x = 0;
		//				int int_rand_y = 0;						
		//				while(1)
		//				{
		//					int_rand_x = rand()%(WIDTH-SQ_WIDTH+1);
		//					int_rand_y = rand()%(HEIGHT-SQ_WIDTH+1);
		//					// Function don't have to lock/unlock mutex
		//					bool b_res = detect_collision(arr_Players,
		//						MAX_PLAYERS, i+1, int_rand_x, int_rand_y);
		//					// If no collision detected
		//					if(b_res == false)
		//					{
		//						break;
		//					}
		//				}
		//				
		//				// Set the coords
		//				arr_Players[i].x = int_rand_x;
		//				arr_Players[i].y = int_rand_y;
		//				
		//				// Set player ID to be the index plus 1
		//				/* pthread_mutex_lock(&mutex); */
		//				int_new_player_id = i+1;
		//				/* printf("int_new_player_id=%d\n", int_new_player_id); */
		//				/* pthread_mutex_unlock(&mutex); */
		//				break;
		//			}
		//		}
		//		pthread_mutex_unlock(&mutex);
		//	}
		//}		
	}
	
	return 0;
}
