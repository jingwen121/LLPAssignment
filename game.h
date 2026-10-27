/* game.h - Coin Rush game logic (server & game logic) */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

/* ---------- Arena ---------- */
#define WIDTH   800
#define HEIGHT  600

/* ---------- Players ---------- */
#define MAX_PLAYERS      5
#define MIN_PLAYERS      2   /* players needed to start a round */
#define PLAYER_W         50
#define PLAYER_H         50
#define PLAYER_Y         (HEIGHT - PLAYER_H - 10) /* players live on the bottom row */
#define PLAYER_SPEED     6   /* pixels per tick */
#define INPUT_TIMEOUT_MS 60  /* no input for this long = key released */

/* ---------- Coins ---------- */
#define MAX_COINS        8
#define COIN_SIZE        20
#define COIN_VALUE       1
#define COIN_SPEED_MIN   2  /* pixels per tick */
#define COIN_SPEED_MAX   4
#define GOLD_SIZE        14  /* gold: smaller, faster, worth more */
#define GOLD_VALUE       3
#define GOLD_SPEED_MIN   5
#define GOLD_SPEED_MAX   7
#define GOLD_CHANCE      10 /* 1 in N spawns is gold (max 1 at a time) */

/* ---------- Timing ---------- */
#define TICK_MS          16 /* ~60 game updates per second */
#define ROUND_SECONDS    60
#define RESULTS_SECONDS  5

typedef enum
{
    PHASE_WAITING = 0, /* lobby: waiting for MIN_PLAYERS */
    PHASE_PLAYING = 1, /* round in progress, coins falling */
    PHASE_RESULTS = 2  /* round over, winners shown */
} Phase;

typedef struct
{
    bool     active;
    int      x, y; /* top-left corner of the square */
    int      score;
    int      dir;  /* latest input: -1 left, 0 none, +1 right */
    uint64_t last_input_ms;  /* when that input arrived */
} Player;

typedef struct
{
    bool active;
    bool gold;
    int  x, y; /* top-left corner */
    int  size;
    int  speed; /* pixels per tick */
    int  value;  /* points when caught */
} Coin;

typedef struct
{
    Phase    phase;
    uint32_t tick;  /* increases every game_tick() */
    uint64_t phase_end_ms;  /* when the current round / results screen ends */
    int      time_left;  /* whole seconds left, for the HUD */
    uint8_t  winners;  /* bitmask: bit 0 = player 1 ... ties set several bits */
    int      best_score;
    Player   players[MAX_PLAYERS]; /* player ID = index + 1 */
    Coin     coins[MAX_COINS];
} Game;

/* Reset everything to an empty lobby. */
void game_init(Game *g);

/* Add a player. Returns the new player ID (1..MAX_PLAYERS), or 0 if the game is full. */
int  game_add_player(Game *g);

/* Remove a player (leave / disconnect). Returns false for an invalid or inactive ID. */
bool game_remove_player(Game *g, int id);

/* Server-auth: Store a player's input. dir must be -1, 0 or +1.
 * Returns false (and ignores the input) if the ID or direction is invalid. */
bool game_set_input(Game *g, int id, int dir, uint64_t now_ms);

/* Advance the game by one tick. Call every TICK_MS milliseconds. */
void game_tick(Game *g, uint64_t now_ms);

/* Number of players currently in the game. */
int  game_num_active(const Game *g);

/* Debug: print phase, timer, scores and coins to the terminal. */
void game_print(const Game *g);

#endif 
