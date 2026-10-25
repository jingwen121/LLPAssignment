/* game.c - Coin Rush game logic */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Helpers
 * ====================================================================== */

/* Random integer in [lo, hi] inclusive. */
static int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

/* Coin collision: rect overlap test (touching edges do NOT count). */
static bool rects_overlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw &&
           ay < by + bh && by < ay + ah;
}

/* Server-auth: Would player i overlap another player if moved to x?
   Used so players cannot walk through each other. */
static bool player_hits_other(const Game *g, int i, int x)
{
    for (int j = 0; j < MAX_PLAYERS; j++)
    {
        if (j == i || !g->players[j].active)
            continue;
        if (rects_overlap(x, PLAYER_Y, PLAYER_W, PLAYER_H, g->players[j].x, PLAYER_Y, PLAYER_W, PLAYER_H))
            return true;
    }
    return false;
}

static int count_gold(const Game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_COINS; i++)
        if (g->coins[i].active && g->coins[i].gold)  n++;
    return n;
}

/*  Coin spawning
 * (Re)spawn coin c above the top of the screen with a random x position.
 *   - About 1 in GOLD_CHANCE spawns is gold, but only if no gold coin is
 *     already on screen (keeps gold rare and worth chasing).
 *   - Gold coins are smaller, faster and worth GOLD_VALUE points.
 *   - Speed is random within a range, so coins don't fall in lockstep.
 * spread_out = true when starting a round, so coins don't all arrive together.
 * Called at round start, when a coin is caught, and when it falls off the bottom. */
static void spawn_coin(Game *g, Coin *c, bool spread_out)
{
    c->active = false;   /* so count_gold() doesn't count this coin itself */

    /* At most one gold coin on screen at a time */
    bool gold = (count_gold(g) == 0) && (rand() % GOLD_CHANCE == 0);

    c->active = true;
    c->gold = gold;
    c->size = gold ? GOLD_SIZE  : COIN_SIZE;
    c->value = gold ? GOLD_VALUE : COIN_VALUE;
    c->speed = gold ? rand_range(GOLD_SPEED_MIN, GOLD_SPEED_MAX)
                     : rand_range(COIN_SPEED_MIN, COIN_SPEED_MAX);
    c->x = rand_range(0, WIDTH - c->size);
    c->y = -c->size - (spread_out ? rand_range(0, HEIGHT) : rand_range(0, 100));
}

static void print_winners(uint8_t mask)
{
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (mask & (1u << i))
            printf("%sPlayer %d", count++ ? " & " : "", i + 1);
    printf(count > 1 ? " (tie)\n" : "\n");
}

/* ======================================================================
 * Per-tick updates
 * ====================================================================== */

/* Move every player by their latest input. The server decides the
 * movement, so a client cannot teleport or move faster than PLAYER_SPEED. */
static void move_players(Game *g, uint64_t now_ms)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        // p points to this player's data, so p->x is the same as g->players[i].x
        Player* p = &g->players[i];

        // Skip empty slots: nobody has joined with this ID
        if (!p->active)
            continue;

        /* The client sends an input about every 10 ms WHILE a key is held,
         * and sends nothing when it is released (there is no "released"
         * message). So if the last input is older than INPUT_TIMEOUT_MS
         * (60 ms), we treat the key as released and the player stops.
         * Without this, players would keep sliding after letting go. */
        int dir = (now_ms - p->last_input_ms <= INPUT_TIMEOUT_MS) ? p->dir : 0;
        if (dir == 0)
            continue;

        int nx = p->x + dir * PLAYER_SPEED;

        /* Keep the square fully inside the window.
         * Left edge: x cannot be below 0.
         * Right edge: x is the LEFT side of the square, so the furthest
         * it can go is WIDTH - PLAYER_W (800 - 50 = 750). */
        if (nx < 0)                nx = 0;
        if (nx > WIDTH - PLAYER_W) nx = WIDTH - PLAYER_W;

        /* Block the move if it walks into another player.
         * (If already overlapping, e.g. after a join, allow moving apart.) */
        if (player_hits_other(g, i, nx) && !player_hits_other(g, i, p->x))
            continue;

        p->x = nx;
    }
}

/* Coins fall; any coin that leaves the bottom respawns at the top. */
static void move_coins(Game *g)
{
    for (int i = 0; i < MAX_COINS; i++)
    {
        Coin *c = &g->coins[i];
        if (!c->active)
            continue;
        c->y += c->speed;
        if (c->y > HEIGHT)
            spawn_coin(g, c, false);
    }
}

/* Player vs coin collision. Lower player IDs are checked first, so if two
   players touch the same coin in the same tick, the lower ID gets it. */
static void check_catches(Game *g)
{
    for (int c = 0; c < MAX_COINS; c++)
    {
        Coin *coin = &g->coins[c];
        if (!coin->active)
            continue;

        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            Player *p = &g->players[i];

            // Skip empty slots: nobody has joined with this ID
            if (!p->active)
                continue;

            /* Collision test */
            if (rects_overlap(p->x, p->y, PLAYER_W, PLAYER_H,
                              coin->x, coin->y, coin->size, coin->size))
            {
                p->score += coin->value;
                printf("Player %d caught a %s coin (+%d) -> score %d\n",
                       i + 1, coin->gold ? "GOLD" : "normal", coin->value, p->score);

                // The caught coin disappears and comes back as a new coin
                // at the top of the screen (may now be gold or normal)
                spawn_coin(g, coin, false);
                break;   /* coin is gone, move on to the next coin */
            }
        }
    }
}

/* ======================================================================
 * Phase changes
 * ====================================================================== */

 /*  WAITING -> PLAYING. Called once by game_tick() when at least
  * MIN_PLAYERS (2) have joined. Sets up a fresh round. */
static void start_round(Game *g, uint64_t now_ms)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        g->players[i].score = 0;

    for (int i = 0; i < MAX_COINS; i++)
        spawn_coin(g, &g->coins[i], true);

    g->winners = 0;
    g->best_score = 0;
    g->phase = PHASE_PLAYING;

    /* Start the 60-second timer.
     * We store WHEN the round must end (now + 60,000 ms) instead of
     * counting down. game_tick() ends the round once the clock reaches
     * this time. if a tick runs late, a countdown would drift and
     * the round would last longer than 60 s; comparing with the real
     * clock always gives exactly 60 s.
     * (uint64_t) makes the multiplication use 64-bit numbers, so it
     * cannot overflow when added to the current time. */

    g->phase_end_ms = now_ms + (uint64_t)ROUND_SECONDS * 1000;
    printf("=== Round started with %d players ===\n", game_num_active(g));
}

/* Find the highest score among active players. Every player with that
 * score gets their bit set in g->winners, so ties are handled naturally. */
static void end_round(Game *g, uint64_t now_ms)
{
    int best = -1; // highest score found so far (-1 = none yet)
    uint8_t mask = 0;  // which players have that score, one bit per player

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!g->players[i].active)
            continue; // skip empty slots

        if (g->players[i].score > best) // new highest score
        {
            best = g->players[i].score;
            mask = (uint8_t)(1u << i);  // forget old winners, only this player
        }
        else if (g->players[i].score == best)  // equal to highest = tie
        {
            mask |= (uint8_t)(1u << i); // add this player alongside
        }
    }

    for (int i = 0; i < MAX_COINS; i++)
        g->coins[i].active = false;

    g->best_score = best < 0 ? 0 : best;
    g->winners = mask;
    g->phase = PHASE_RESULTS;
    g->phase_end_ms = now_ms + (uint64_t)RESULTS_SECONDS * 1000;

    printf("=== Round over! Best score %d. Winner: ", g->best_score);
    print_winners(mask);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void game_init(Game *g)
{
    memset(g, 0, sizeof(*g));
    g->phase = PHASE_WAITING;
}

int game_num_active(const Game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].active)
            n++;
    return n;
}

int game_add_player(Game *g)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        Player *p = &g->players[i];

        // Slot already taken by another player, try the next one
        if (p->active)
            continue;

        // Free slot found. Wipe it to all zeros first, so nothing from a
        // previous player who used this slot (score, direction, last
        // input time) carries over to the new player.
        memset(p, 0, sizeof(*p));


        p->active = true;
        
        /* Start position: split the window into MAX_PLAYERS equal lanes
         * (800 / 5 = 160 px each) and centre the player in its own lane.
         *   i * 160 = left edge of this player's lane
         *   (160 - 50) / 2 = 55, the gap that centres a 50 px square
         * So players start at x = 55, 215, 375, 535, 695.
         *  each ID has its own lane, so new players never start on top of each other. */
        p->x = i * (WIDTH / MAX_PLAYERS) + (WIDTH / MAX_PLAYERS - PLAYER_W) / 2;
        p->y = PLAYER_Y;// Everyone stands on the bottom row and only moves left/right
        printf("Player %d joined (%d active)\n", i + 1, game_num_active(g));
        return i + 1;   /* player ID = index + 1 */
    }
    printf("Join rejected: game full\n");
    return 0;
}

bool game_remove_player(Game *g, int id)
{
    if (id < 1 || id > MAX_PLAYERS || !g->players[id - 1].active)
        return false;
    g->players[id - 1].active = false;
    printf("Player %d left (%d active)\n", id, game_num_active(g));
    return true;
}

bool game_set_input(Game *g, int id, int dir, uint64_t now_ms)
{
    /* Input validation: never trust the client.
     * Bad IDs would otherwise index outside the players array. */
    if (id < 1 || id > MAX_PLAYERS) return false;
    if (!g->players[id - 1].active)  return false;
    if (dir < -1 || dir > 1)  return false;

    g->players[id - 1].dir  = dir;
    g->players[id - 1].last_input_ms = now_ms;
    return true;
}

void game_tick(Game *g, uint64_t now_ms)
{
    g->tick++;

    /* Players may move in every phase (so they can warm up in the lobby) */
    move_players(g, now_ms);

    switch (g->phase)
    {
        case PHASE_WAITING:
            if (game_num_active(g) >= MIN_PLAYERS)
                start_round(g, now_ms); // PLAYING, starts the 60 s timer
            break;

        case PHASE_PLAYING:
            move_coins(g);
            check_catches(g);
            
            /*  Round timer:End the round when:
             *   - the clock has reached the end time set in start_round(), or
             *   - players left and fewer than MIN_PLAYERS remain
             *  (one player alone can't play, so they win) */
            if (now_ms >= g->phase_end_ms || game_num_active(g) < MIN_PLAYERS)
                end_round(g, now_ms);
            break;

        case PHASE_RESULTS:
            if (now_ms >= g->phase_end_ms)
            {
                // Results time is up: back to the lobby and clear winners.
                // If 2+ players are still here, the NEXT tick starts a new
                // round automatically, so the game keeps going.
                g->phase   = PHASE_WAITING;
                g->winners = 0;
                printf("=== Back to lobby ===\n");
            }
            break;
    }

    /* Seconds left for the HUD (rounded up, so it shows 60 .. 1) */
    g->time_left = (g->phase != PHASE_WAITING && g->phase_end_ms > now_ms)
                   ? (int)((g->phase_end_ms - now_ms + 999) / 1000)
                   : 0;
}

void game_print(const Game *g)
{
    static const char *names[] = { "WAITING", "PLAYING", "RESULTS" };
    int coins = 0;
    for (int i = 0; i < MAX_COINS; i++)
        if (g->coins[i].active)
            coins++;

    printf("[%s] time left %2ds | coins %d | ", names[g->phase], g->time_left, coins);

    // Position and score of every player in the game
    // (%3d pads x to 3 characters, so columns line up)
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].active)
            printf("P%d: x=%3d score=%d  ", i + 1, g->players[i].x, g->players[i].score);
    printf("\n");
}
