/* game.c - Coin Rush game logic */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* x86/x86-64 builds use explicit SSE2 for player-vs-object collision checks.
 * Other architectures keep the same scalar implementation and behavior. */
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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
    int i = 0;

    /* Four-way source unrolling reduces loop overhead and the dependency
     * chain in this small, frequently-used fixed-size count. */
    for (; i + 3 < MAX_COINS; i += 4)
    {
        if (g->coins[i].active && g->coins[i].gold)  n++;
        if (g->coins[i + 1].active && g->coins[i + 1].gold)  n++;
        if (g->coins[i + 2].active && g->coins[i + 2].gold)  n++;
        if (g->coins[i + 3].active && g->coins[i + 3].gold)  n++;
    }
    for (; i < MAX_COINS; ++i)
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

static void print_winners(const Game *g, uint8_t mask)
{
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (mask & (1u << i))
            printf("%s%s", count++ ? " & " : "", g->players[i].name);
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
        if (!p->active || p->lives <= 0)
            continue;
        if (g->level == 4 && !(g->contenders & (1u << i)))
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
static inline void move_one_coin(Game *g, Coin *c)
{
    if (!c->active)
        return;
    c->y += c->speed;
    if (c->y > HEIGHT)
        spawn_coin(g, c, false);
}

static void move_coins(Game *g)
{
    int i = 0;

    /* Manually unroll four coin updates per iteration. Keep the updates
     * in index order because a respawn consumes the shared RNG sequence. */
    for (; i + 3 < MAX_COINS; i += 4)
    {
        move_one_coin(g, &g->coins[i]);
        move_one_coin(g, &g->coins[i + 1]);
        move_one_coin(g, &g->coins[i + 2]);
        move_one_coin(g, &g->coins[i + 3]);
    }

    for (; i < MAX_COINS; ++i)
        move_one_coin(g, &g->coins[i]);
}

static void move_bombs(Game *g)
{
    if (g->level < 2 || g->level == 4)
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

/* Return the lowest player index whose rectangle overlaps an object.
 * Four player rectangles are checked together with SSE2 on x86/x86-64;
 * unsupported architectures use the equivalent scalar path. The lowest
 * set lane is selected to preserve the game's lower-player-ID priority. */
static int find_overlapping_player(const Game *g, int object_x, int object_y,
                                   int object_size, uint64_t now_ms,
                                   bool enforce_bomb_cooldown)
{
    int i = 0;

#if defined(__SSE2__)
    for (; i + 3 < MAX_PLAYERS; i += 4)
    {
        int player_left[4] = {0, 0, 0, 0};
        int player_right[4] = {0, 0, 0, 0};
        int player_top[4] = {0, 0, 0, 0};
        int player_bottom[4] = {0, 0, 0, 0};
        int eligible[4] = {0, 0, 0, 0};

        for (int lane = 0; lane < 4; ++lane)
        {
            const Player *p = &g->players[i + lane];
            bool ready = p->active && p->lives > 0 && (g->level != 4 || (g->contenders & (1u << (i + lane))));
            if (ready && enforce_bomb_cooldown && p->last_bomb_hit_ms != 0 &&
                now_ms - p->last_bomb_hit_ms < BOMB_HIT_COOLDOWN_MS)
                ready = false;

            if (ready)
            {
                player_left[lane] = p->x;
                player_right[lane] = p->x + PLAYER_W;
                player_top[lane] = p->y;
                player_bottom[lane] = p->y + PLAYER_H;
                eligible[lane] = -1;
            }
        }

        const __m128i left = _mm_loadu_si128((const __m128i *)player_left);
        const __m128i right = _mm_loadu_si128((const __m128i *)player_right);
        const __m128i top = _mm_loadu_si128((const __m128i *)player_top);
        const __m128i bottom = _mm_loadu_si128((const __m128i *)player_bottom);
        const __m128i valid = _mm_loadu_si128((const __m128i *)eligible);

        __m128i overlap = _mm_cmplt_epi32(left,
                                           _mm_set1_epi32(object_x + object_size));
        overlap = _mm_and_si128(overlap,
                                 _mm_cmplt_epi32(_mm_set1_epi32(object_x), right));
        overlap = _mm_and_si128(overlap,
                                 _mm_cmplt_epi32(top,
                                                 _mm_set1_epi32(object_y + object_size)));
        overlap = _mm_and_si128(overlap,
                                 _mm_cmplt_epi32(_mm_set1_epi32(object_y), bottom));
        overlap = _mm_and_si128(overlap, valid);

        unsigned lanes = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(overlap));
        if (lanes != 0)
        {
#if defined(__GNUC__) || defined(__clang__)
            return i + __builtin_ctz(lanes);
#else
            for (int lane = 0; lane < 4; ++lane)
                if (lanes & (1u << lane)) return i + lane;
#endif
        }
    }
#endif

    /* Scalar tail (one player for the current five-player limit), also used
     * as the complete implementation on non-SSE2 architectures. */
    for (; i < MAX_PLAYERS; ++i)
    {
        const Player *p = &g->players[i];
        if (!p->active || p->lives <= 0 || (g->level == 4 && !(g->contenders & (1u << i))))
            continue;
        if (enforce_bomb_cooldown && p->last_bomb_hit_ms != 0 &&
            now_ms - p->last_bomb_hit_ms < BOMB_HIT_COOLDOWN_MS)
            continue;
        if (rects_overlap(p->x, p->y, PLAYER_W, PLAYER_H,
                          object_x, object_y, object_size, object_size))
            return i;
    }
    return -1;
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
        int player_index = find_overlapping_player(g, coin->x, coin->y,
                                                   coin->size, 0, false);
        if (player_index < 0)
            continue;

        Player *p = &g->players[player_index];
        if (g->level == 4) g->bonus_score[player_index] += coin->value;
        else p->score += coin->value;
        ++p->collect_seq;
        printf("%s caught a %s coin (+%d) -> score %d\n",
               p->name, coin->gold ? "GOLD" : "normal",
               coin->value, p->score);

        /* Respawn after selecting the lowest-ID overlapping player. */
        spawn_coin(g, coin, false);
    }
}

static void check_bomb_hits(Game *g, uint64_t now_ms)
{
    if (g->level < 2 || g->level == 4)
        return;

    for (int b = 0; b < MAX_BOMBS; ++b)
    {
        Bomb *bomb = &g->bombs[b];
        if (!bomb->active)
            continue;
        int player_index = find_overlapping_player(g, bomb->x, bomb->y,
                                                   bomb->size, now_ms, true);
        if (player_index < 0)
            continue;

        Player *p = &g->players[player_index];
        if (++p->bomb_hits >= (g->level == 3 ? 3 : 1)) {
            p->bomb_hits = 0;
            if (p->lives > 0) --p->lives;
        }
        ++p->bomb_seq;
        p->last_bomb_hit_ms = now_ms;
        printf("%s hit a bomb -> %d lives\n",
               p->name, p->lives);
        spawn_bomb(g, bomb, false);
    }
}

/* ======================================================================
 * Phase changes
 * ====================================================================== */

 /*  WAITING -> PLAYING. Called once by game_tick() when at least
  * MIN_PLAYERS (2) have joined. Sets up a fresh round. */
static void start_round(Game *g, uint64_t now_ms)
{
    g->ready_players = 0;
    g->level = 1;
    g->contenders = 0;
    g->danger_lane = -1;
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
        g->players[i].bomb_hits = 0;
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

static void start_next_level(Game *g, uint64_t now_ms)
{
    g->ready_players = 0;
    ++g->level;
    g->level_start_tick = g->tick;
    g->danger_lane = -1;
    g->phase = PHASE_PLAYING;
    g->phase_end_ms = now_ms + (uint64_t)LEVEL_TWO_SECONDS * 1000;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (!g->players[i].active)
            continue;
        g->players[i].lives = LIVES_PER_PLAYER;
        g->players[i].last_bomb_hit_ms = 0;
        g->players[i].bomb_hits = 0;
    }
    for (int i = 0; i < MAX_COINS; ++i)
        spawn_coin(g, &g->coins[i], true);
    for (int i = 0; i < MAX_BOMBS; ++i)
        spawn_bomb(g, &g->bombs[i], true);

    printf("=== Level %d started: %d bomb hits cost 1 life ===\n",
           g->level, g->level == 3 ? 3 : 1);
}

/* Find the highest score among active players. Every player with that
 * score gets their bit set in g->winners, so ties are handled naturally. */
static void end_round(Game *g, uint64_t now_ms)
{
    int best = -1; // highest score found so far (-1 = none yet)
    uint8_t mask = 0;  // which players have that score, one bit per player

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!g->players[i].active || (g->level == 4 && !(g->contenders & (1u << i))))
            continue; // skip empty slots

        int score = g->level == 4 ? g->bonus_score[i] : g->players[i].score;
        if (score > best) // new highest score
        {
            best = score;
            mask = (uint8_t)(1u << i);  // forget old winners, only this player
        }
        else if (score == best)  // equal to highest = tie
        {
            mask |= (uint8_t)(1u << i); // add this player alongside
        }
    }

    if (g->level >= 3 && mask && (mask & (mask - 1))) {
        g->contenders = mask;
        g->winners = mask;
        g->level = 4;
        g->danger_lane = -1;
        memset(g->bonus_score, 0, sizeof(g->bonus_score));
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            g->players[i].x = i * (WIDTH / MAX_PLAYERS) + 55;
            g->players[i].dir = 0;
            g->players[i].lives = LIVES_PER_PLAYER;
            g->players[i].bomb_hits = 0;
        }
        for (int i = 0; i < MAX_COINS; ++i) spawn_coin(g, &g->coins[i], true);
        for (int i = 0; i < MAX_BOMBS; ++i) g->bombs[i].active = false;
        g->phase = PHASE_PLAYING;
        g->phase_end_ms = now_ms + BONUS_SECONDS * 1000;
        return;
    }
    for (int i = 0; i < MAX_COINS; i++)
        g->coins[i].active = false;
    for (int i = 0; i < MAX_BOMBS; i++)
        g->bombs[i].active = false;

    g->best_score = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (mask & (1u << i)) { g->best_score = g->players[i].score; break; }
    g->winners = mask;
    g->phase = PHASE_RESULTS;
    g->ready_players = 0;
    g->phase_end_ms = 0;

    printf("=== Round over! Best score %d. Winner: ", g->best_score);
    print_winners(g, mask);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void game_init(Game *g)
{
    memset(g, 0, sizeof(*g));
    g->phase = PHASE_WAITING;
    g->danger_lane = -1;
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
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (g->players[i].active && g->players[i].lives > 0) ++n;
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


        snprintf(p->name, sizeof(p->name), "Player %d", i + 1);
        p->active = true;
        g->ready_players &= (uint8_t)~(1u << i);
        p->lives = LIVES_PER_PLAYER;
        
        /* Start position: split the window into MAX_PLAYERS equal lanes
         * (800 / 5 = 160 px each) and centre the player in its own lane.
         *   i * 160 = left edge of this player's lane
         *   (160 - 50) / 2 = 55, the gap that centres a 50 px square
         * So players start at x = 55, 215, 375, 535, 695.
         *  each ID has its own lane, so new players never start on top of each other. */
        p->x = i * (WIDTH / MAX_PLAYERS) + (WIDTH / MAX_PLAYERS - PLAYER_W) / 2;
        p->y = PLAYER_Y;// Everyone stands on the bottom row and only moves left/right
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
    g->ready_players &= (uint8_t)~(1u << (id - 1));
    printf("%s left (%d active)\n", g->players[id - 1].name, game_num_active(g));
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

static void advance_when_ready(Game *g, uint64_t now_ms)
{
    uint8_t active = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (g->players[i].active) active |= (uint8_t)(1u << i);
    if (game_num_active(g) < MIN_PLAYERS || (g->ready_players & active) != active) return;
    if (g->phase == PHASE_LEVEL_BREAK) start_next_level(g, now_ms);
    else if (g->phase == PHASE_RESULTS || g->phase == PHASE_WAITING) start_round(g, now_ms);
}

bool game_continue_level(Game *g, int player_id, uint64_t now_ms)
{
    if ((g->phase != PHASE_LEVEL_BREAK && g->phase != PHASE_RESULTS && g->phase != PHASE_WAITING) || player_id < 1 ||
        player_id > MAX_PLAYERS || !g->players[player_id - 1].active)
        return false;
    g->ready_players |= (uint8_t)(1u << (player_id - 1));
    advance_when_ready(g, now_ms);
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
            advance_when_ready(g, now_ms);
            break;

        case PHASE_PLAYING:
            if (game_num_active(g) < MIN_PLAYERS) { end_round(g, now_ms); break; }
            if (g->level == 3) {
                uint32_t elapsed = g->tick - g->level_start_tick;
                unsigned period = 240;
                if (elapsed >= 1250) { elapsed -= 1250; period = 180; }
                unsigned step = elapsed % period;
                int lane = (int)((elapsed / period) % 4);
                g->danger_lane = step < 94 ? lane : -1;
                if (step == 1) {
                    Coin *c = &g->coins[0];
                    c->active = true; c->gold = true; c->size = GOLD_SIZE;
                    c->value = GOLD_VALUE; c->speed = 5;
                    c->x = lane * 200 + 90; c->y = 80;
                    for (int i = 1; i < MAX_COINS; ++i)
                        if (g->coins[i].gold) { g->coins[i].gold = false; g->coins[i].size = COIN_SIZE; g->coins[i].value = COIN_VALUE; }
                }
                if (step == 94) {
                    for (int i = 0; i < MAX_BOMBS; ++i) {
                        Bomb *b = &g->bombs[i]; spawn_bomb(g, b, false);
                        b->x = lane * 200 + i * 48; b->y = PLAYER_Y - 130; b->speed = 10;
                    }
                }
            }
            move_coins(g);
            check_catches(g);
            move_bombs(g);
            check_bomb_hits(g, now_ms);

            if (g->level < 3 && (now_ms >= g->phase_end_ms ||
                (g->level == 2 && game_num_alive(g) == 0)))
            {
                g->ready_players = 0;
                g->phase = PHASE_LEVEL_BREAK;
                g->phase_end_ms = 0;
                for (int i = 0; i < MAX_COINS; ++i)
                    g->coins[i].active = false;
                for (int i = 0; i < MAX_BOMBS; ++i)
                    g->bombs[i].active = false;
                printf("=== Level %d complete: waiting for Continue ===\n", g->level);
            }
            else if (g->level >= 3 && (now_ms >= g->phase_end_ms ||
                     (g->level == 3 && game_num_alive(g) == 0)))
                end_round(g, now_ms);
            break;

        case PHASE_LEVEL_BREAK:
            if (game_num_active(g) < MIN_PLAYERS)
                end_round(g, now_ms);
            else advance_when_ready(g, now_ms);
            break;

        case PHASE_RESULTS:
            advance_when_ready(g, now_ms);
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
            printf("%s: x=%3d score=%d lives=%d  ", g->players[i].name, g->players[i].x,
                   g->players[i].score, g->players[i].lives);
    printf("\n");
}
