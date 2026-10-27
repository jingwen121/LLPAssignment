// Build with make and run ./build/client credentials/client1

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS client
2. Supports max 5 clients
3. Start the server before starting clients
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include "network.h"
#include "dds/dds.h"
#include "messages.h"
#include "stb_easy_font.h"

#define STB_EASY_FONT_IMPLEMENTATION

#define MAX_SAMPLES 1
#define MAX_PLAYERS 5


#define WIDTH  800
#define HEIGHT 600
#define SQ_WIDTH 50


// Player coord defined as NW corner of square
typedef struct
{
    bool b_active;
    int x;
    int y;
    int score;
} Player;

// ### GLOBAL VARIABLES ###
int int_player_id = -1;

// Table to track clients
// Init all members to 0
static Player arr_Players[MAX_PLAYERS] = {0};

// Direction - n means no direction
char ch_dir = 'n';
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// OpenGL stays on main; this worker owns all DDS handles; atomic flags carry
// shutdown and startup failure without racing with the window loop
static atomic_bool running = true;
static atomic_int worker_status = 0;

// Read this complete snapshot under mutex for the HUD,
// coins and result screen; keep rendering based on server data, including scores
static GameState game_state;


void draw_text(float x, float y, float scale, const char *text)
{
    char buffer[4096];

    int num_quads = stb_easy_font_print(
        0, 0,
        (char *)text,
        NULL,
        buffer,
        sizeof(buffer)
    );

    glPushMatrix();

    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);

    glEnableClientState(GL_VERTEX_ARRAY);

    glVertexPointer(2, GL_FLOAT, 16, buffer);
    glDrawArrays(GL_QUADS, 0, num_quads * 4);

    glDisableClientState(GL_VERTEX_ARRAY);

    glPopMatrix();
}

void *legacy_worker(void *arg)
{

    // This worker does not use the argument passed by pthread_create
    (void)arg;

	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t input_topic;
	dds_entity_t position_topic;
	dds_entity_t response_reader;
	dds_entity_t position_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;
	
	//Pointer to unknown type
	void *samples_response[MAX_SAMPLES];
	dds_sample_info_t infos_response[MAX_SAMPLES];
	void *samples_position[MAX_SAMPLES];
	dds_sample_info_t infos_position[MAX_SAMPLES];
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
	
	input_topic = dds_create_topic
	(
        participant,
        &Input_desc,
        "Input",
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
	
	/* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    join_writer = dds_create_writer(
        participant,
        join_topic,
        NULL,
        NULL
    );
	
	input_writer = dds_create_writer(
        participant,
        input_topic,
        NULL,
        NULL
    );
	
	/* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    response_reader = dds_create_reader(
        participant,
        response_topic,
        NULL,
        NULL
    );}

void *worker(void *arg)
{
    Network net;

    // Use only this client's certificate directory when creating the participant
    if (!net_open(&net, false, arg))
    {
        atomic_store(&worker_status, 1);
        atomic_store(&running, false);
        return NULL;
    }

    while (atomic_load(&running))
    {
        // Take a small copy of the current input intent; never keep mutex locked across
        // DDS operations; discovery or reliable delivery can take time to finish
        pthread_mutex_lock(&mutex);
        char direction = ch_dir;
        pthread_mutex_unlock(&mutex);

        // Poll before sending so a newly assigned ID and admission token are
        // available to the next heartbeat; no position or score is sent by us
        GameState snapshot;
        bool updated = net_client_state(&net, &snapshot);
        if (!net_client_send(&net, direction))
        {
            atomic_store(&worker_status, 1);
            atomic_store(&running, false);
        }
	
        pthread_mutex_lock(&mutex);
        int_player_id = net.player_id;
	
        if (updated)
        {
            // Replace the entire display state; copying only active players
            // would leave a disconnected player's old square visible forever
            game_state = snapshot;
            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                arr_Players[i] = (Player){snapshot.players[i].active,
                    snapshot.players[i].x, snapshot.players[i].y,
                    snapshot.players[i].score};
            }
        }
	
        if (int_player_id == -1)
        {
            // A timeout invalidates the assignment; clear stale data instead of
            // making a disconnected client look as if it is still in the round
            memset(arr_Players, 0, sizeof(arr_Players));
            memset(&game_state, 0, sizeof(game_state));
        }
        pthread_mutex_unlock(&mutex);
	
        dds_sleepfor(DDS_MSECS(1));
    }

    // Try a reliable leave before deleting DDS; if the window/process disappears
    // without this path, the server's heartbeat timeout still releases the slot
    net_close(&net);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *credentials = argc > 1 ? argv[1] : "credentials/client1";
	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, (void *)credentials) != 0)
    {
        perror("pthread_create");
        return 1;
    }
	
	
    if (!glfwInit())
    {
        atomic_store(&running, false);
        pthread_join(thread, NULL);
        return 1;
    }

    GLFWwindow *window =
        glfwCreateWindow(WIDTH, HEIGHT, "Coin Rush", NULL, NULL);

    if (!window)
    {
        atomic_store(&running, false);
        pthread_join(thread, NULL);
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    /* Use pixel coordinates */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* glOrtho(0, WIDTH, 0, HEIGHT, -1, 1); */
	glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);

  /*   glMatrixMode(GL_MODELVIEW);
    glLoadIdentity(); */

    while (atomic_load(&running) && !glfwWindowShouldClose(window))
    {
		 /* Process keyboard/window events */
        glfwPollEvents();

        /* Non-blocking keyboard input */
		if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'l';
			pthread_mutex_unlock(&mutex);
		}
		
		else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'r';
			pthread_mutex_unlock(&mutex);
		}
		
		else
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'n';
			pthread_mutex_unlock(&mutex);
		}
		
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
		glColor3f(1.0f, 0.0f, 0.0f);
		
		
		char str_banner[100] = "Waiting for server ...";
		pthread_mutex_lock(&mutex);

		// Copy the player ID while the worker cannot change it
		int display_player_id = int_player_id;
		pthread_mutex_unlock(&mutex);

		// Use the copied player ID when creating the banner text
		if (display_player_id != -1)
		{
			snprintf(str_banner, sizeof(str_banner), "Player %d", display_player_id);
		}
		draw_text(320, 20, 3.0f, str_banner);
		
		for(int i = 0; i < MAX_PLAYERS; i++)
		{
			pthread_mutex_lock(&mutex);
			if(arr_Players[i].b_active == true)
			{
				// Label the player ID
				char str_player[2] = "";
				snprintf(str_player, sizeof(str_player), "%d", i+1);
				/* printf("Coords = %d, %d\n",arr_Players[i].x, arr_Players[i].y);				 */
				/* draw_text(100, 400, 3.0f, str_banner); */
				
				draw_text(arr_Players[i].x, arr_Players[i].y, 3.0f, str_player);				
				
				glBegin(GL_LINE_LOOP);
					glVertex2f(arr_Players[i].x, arr_Players[i].y);
					glVertex2f(arr_Players[i].x, arr_Players[i].y + SQ_WIDTH);
					glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y + SQ_WIDTH);
					glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y);
				glEnd();
			}
			pthread_mutex_unlock(&mutex);			
		}
        glfwSwapBuffers(window);
    }

    // Stop the networking worker before freeing the window
    atomic_store(&running, false);
    pthread_join(thread, NULL);

    glfwDestroyWindow(window);
    glfwTerminate();

    return atomic_load(&worker_status);
}
