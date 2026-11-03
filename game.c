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

static int count_gold(const Game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_COINS; i++)
        if (g->coins[i].active && g->coins[i].gold)  n++;
    return n;
}

static void set_coin_speed(Game *g, Coin *c)
{
    if (c->gold)
        c->speed = g->level >= 2 ? rand_range(7, 10) :
                                  rand_range(GOLD_SPEED_MIN, GOLD_SPEED_MAX);
    else
        c->speed = g->level >= 2 ? rand_range(4, 6) :
                                  rand_range(COIN_SPEED_MIN, COIN_SPEED_MAX);
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
    set_coin_speed(g, c);
    c->x = rand_range(0, WIDTH - c->size);
    c->y = -c->size - (spread_out ? rand_range(0, HEIGHT) : rand_range(0, 100));
}

static void spawn_bomb(Game *g, Bomb *b, bool spread_out)
{
    b->active = true;
    b->size = BOMB_SIZE;
    b->speed = g->level >= 2 ? rand_range(6, 9) : rand_range(3, 5);
    b->x = rand_range(0, WIDTH - b->size);
    b->y = -b->size - (spread_out ? rand_range(0, HEIGHT) : rand_range(0, 220));
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
        if (p->lives <= 0)
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

        /* Players share a lane and may walk through one another. */
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

static void move_bombs(Game *g)
{
    if (g->level < 2)
        return;

    for (int i = 0; i < MAX_BOMBS; ++i)
    {
        Bomb *b = &g->bombs[i];
        if (!b->active)
            continue;
        b->y += b->speed;
        if (b->y > HEIGHT)
            spawn_bomb(g, b, false);
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
            if (!p->active || p->lives <= 0)
                continue;

            /* Collision test */
            if (rects_overlap(p->x, p->y, PLAYER_W, PLAYER_H,
                              coin->x, coin->y, coin->size, coin->size))
            {
                p->score += coin->value;
                ++p->collect_seq;
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

static void check_bomb_hits(Game *g, uint64_t now_ms)
{
    if (g->level < 2)
        return;

    for (int b = 0; b < MAX_BOMBS; ++b)
    {
        Bomb *bomb = &g->bombs[b];
        if (!bomb->active)
            continue;

        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            Player *p = &g->players[i];
            if (!p->active || p->lives <= 0 ||
                (p->last_bomb_hit_ms != 0 &&
                 now_ms - p->last_bomb_hit_ms < BOMB_HIT_COOLDOWN_MS))
                continue;

            if (!rects_overlap(p->x, p->y, PLAYER_W, PLAYER_H,
                               bomb->x, bomb->y, bomb->size, bomb->size))
                continue;

            --p->lives;
            p->last_bomb_hit_ms = now_ms;
            printf("Player %d hit a bomb -> %d lives left\n", i + 1, p->lives);
            spawn_bomb(g, bomb, false);
            break;
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
    g->level = 1;
    for (int i = 0; i < MAX_PLAYERS; i++)
        g->players[i].score = 0;

    for (int i = 0; i < MAX_COINS; i++)
        spawn_coin(g, &g->coins[i], true);

    g->winners = 0;
    g->best_score = 0;
    g->phase = PHASE_PLAYING;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        g->players[i].lives = LIVES_PER_PLAYER;
        g->players[i].last_bomb_hit_ms = 0;
    }
    for (int i = 0; i < MAX_BOMBS; ++i)
    {
        /* Level 1 is the original coin-only mode. Bombs begin in level 2. */
        g->bombs[i].active = false;
    }

    /* Level 1 has its own timer. Level 2 starts only when someone chooses
     * Continue from the shared level-break screen. */
    g->phase_end_ms = now_ms + (uint64_t)LEVEL_ONE_SECONDS * 1000;
    printf("=== Round started with %d players ===\n", game_num_active(g));
}

static void start_level_two(Game *g, uint64_t now_ms)
{
    g->level = 2;
    g->phase = PHASE_PLAYING;
    g->phase_end_ms = now_ms + (uint64_t)LEVEL_TWO_SECONDS * 1000;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (!g->players[i].active)
            continue;
        g->players[i].lives = LIVES_PER_PLAYER;
        g->players[i].last_bomb_hit_ms = 0;
    }
    for (int i = 0; i < MAX_COINS; ++i)
        spawn_coin(g, &g->coins[i], true);
    for (int i = 0; i < MAX_BOMBS; ++i)
        spawn_bomb(g, &g->bombs[i], true);

    printf("=== Level 2 started: faster coins, bombs and 3 lives ===\n");
}

/* Find the highest score among active players. Every player with that
 * score gets their bit set in g->winners, so ties are handled naturally. */
static void end_round(Game *g, uint64_t now_ms)
{
    int best = -1; // highest score found so far (-1 = none yet)
    uint8_t mask = 0;  // which players have that score, one bit per player

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!g->players[i].active || g->players[i].lives <= 0)
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
    for (int i = 0; i < MAX_BOMBS; i++)
        g->bombs[i].active = false;

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

int game_num_alive(const Game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].active && g->players[i].lives > 0)
            ++n;
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
        p->lives = LIVES_PER_PLAYER;
        
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

bool game_continue_level(Game *g, int player_id, uint64_t now_ms)
{
    if (g->phase != PHASE_LEVEL_BREAK || player_id < 1 ||
        player_id > MAX_PLAYERS || !g->players[player_id - 1].active)
        return false;

    start_level_two(g, now_ms);
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
            move_bombs(g);
            check_bomb_hits(g, now_ms);

            if (g->level == 1 && now_ms >= g->phase_end_ms)
            {
                g->phase = PHASE_LEVEL_BREAK;
                g->phase_end_ms = 0;
                for (int i = 0; i < MAX_COINS; ++i)
                    g->coins[i].active = false;
                for (int i = 0; i < MAX_BOMBS; ++i)
                    g->bombs[i].active = false;
                printf("=== Level 1 complete: waiting for Continue ===\n");
            }
            else if (g->level == 2 &&
                     (now_ms >= g->phase_end_ms || game_num_alive(g) < MIN_PLAYERS))
                end_round(g, now_ms);
            break;

        case PHASE_LEVEL_BREAK:
            if (game_num_active(g) < MIN_PLAYERS)
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
    static const char *names[] = { "WAITING", "PLAYING", "LEVEL BREAK", "RESULTS" };
    int coins = 0;
    for (int i = 0; i < MAX_COINS; i++)
        if (g->coins[i].active)
            coins++;

    printf("[%s] level %d time left %2ds | coins %d | ", names[g->phase], g->level, g->time_left, coins);

    // Position and score of every player in the game
    // (%3d pads x to 3 characters, so columns line up)
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].active)
            printf("P%d: x=%3d score=%d lives=%d  ", i + 1, g->players[i].x,
                   g->players[i].score, g->players[i].lives);
    printf("\n");
}
