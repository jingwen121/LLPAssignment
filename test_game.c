/* test_game.c 
 *
 * Build & run:
 *   gcc -Wall -Wextra -o test_game test_game.c game.c && ./test_game
 *
 * Uses fake time, so a full 60-second round runs in a few milliseconds.
 */
#include "game.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Simple bot: move towards the lowest coin on screen. */
static int bot_dir(const Game *g, int idx)
{
    int best = -1, best_y = -100000;
    for (int c = 0; c < MAX_COINS; c++)
        if (g->coins[c].active && g->coins[c].y > best_y)
        {
            best_y = g->coins[c].y;
            best = c;
        }
    if (best < 0)
        return 0;

    int coin_cx   = g->coins[best].x + g->coins[best].size / 2;
    int player_cx = g->players[idx].x + PLAYER_W / 2;
    if (coin_cx < player_cx - 3) return -1;
    if (coin_cx > player_cx + 3) return 1;
    return 0;
}

/* Run ticks until the phase changes (or a limit is hit). Returns the new time. */
static uint64_t run_until_phase_changes(Game *g, uint64_t t, int use_bots)
{
    Phase start = g->phase;
    for (int n = 0; n < 100000 && g->phase == start; n++)
    {
        t += TICK_MS;
        if (use_bots)
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (g->players[i].active)
                    game_set_input(g, i + 1, bot_dir(g, i), t);
        game_tick(g, t);

        /* Invariants checked every tick */
        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            const Player *p = &g->players[i];
            if (!p->active) continue;
            assert(p->x >= 0 && p->x <= WIDTH - PLAYER_W); /* inside window */
            // Players may overlap after the latest game change; arena bounds still apply
        }
    }
    return t;
}

int main(void)
{
    srand(42);
    Game g;
    uint64_t t = 1000;

    /* 1. Joining and the player limit */
    printf("\n--- Test 1: join / game full / leave ---\n");
    game_init(&g);
    for (int i = 1; i <= MAX_PLAYERS; i++)
        assert(game_add_player(&g) == i);
    assert(game_add_player(&g) == 0); /* 6th player rejected */
    assert(game_remove_player(&g, 5));
    assert(game_add_player(&g) == 5); /* freed slot is reused */
    assert(game_remove_player(&g, 3));
    assert(game_remove_player(&g, 4));
    assert(game_remove_player(&g, 5));
    assert(!game_remove_player(&g, 9)); /* invalid ID */
    assert(!game_remove_player(&g, 3));  /* already gone */
    assert(game_num_active(&g) == 2);

    /* 2. Input validation  */
    printf("\n--- Test 2: input validation ---\n");
    assert(!game_set_input(&g, 0, 1, t));  /* ID too small */
    assert(!game_set_input(&g, 99, 1, t)); /* ID too large */
    assert(!game_set_input(&g, -5, 1, t));  /* negative ID */
    assert(!game_set_input(&g, 3, 1, t));  /* player not in game */
    assert(!game_set_input(&g, 1, 7, t));  /* invalid direction */
    assert(game_set_input(&g, 1, -1, t));    /* valid */
    printf("All bad inputs rejected\n");

    /* 3. Lobby -> round starts */
    printf("\n--- Test 3: round starts with 2 players ---\n");
    t += TICK_MS;
    game_tick(&g, t);
    assert(g.phase == PHASE_PLAYING);
    assert(g.time_left == ROUND_SECONDS);
    for (int c = 0; c < MAX_COINS; c++)
        assert(g.coins[c].active);

    /*  4. Full 60 s round with bots */
    printf("\n--- Test 4: full round with 2 bots ---\n");
    uint64_t round_start = t;
    t = run_until_phase_changes(&g, t, 1);
    assert(g.phase == PHASE_RESULTS);
    assert(t - round_start >= ROUND_SECONDS * 1000);
    int total = g.players[0].score + g.players[1].score;
    printf("Final: P1=%d P2=%d (total %d), winners mask=0x%x\n",
           g.players[0].score, g.players[1].score, total, g.winners);
    assert(total > 0);   /* bots caught something */
    assert(g.winners != 0);
    for (int c = 0; c < MAX_COINS; c++)
        assert(!g.coins[c].active);  /* coins cleared on results */

    /*  5. Results -> lobby -> new round */
    printf("\n--- Test 5: results screen returns to lobby ---\n");
    t = run_until_phase_changes(&g, t, 0);
    assert(g.phase == PHASE_WAITING);
    t += TICK_MS;
    game_tick(&g, t);
    assert(g.phase == PHASE_PLAYING);
    assert(g.players[0].score == 0 && g.players[1].score == 0);  /* scores reset */

    /* 6. Tie handling  */
    printf("\n--- Test 6: tie ---\n");
    for (int c = 0; c < MAX_COINS; c++)
        g.coins[c].active = false;  /* no more catches */
    g.players[0].score = 7;
    g.players[1].score = 7;
    t = g.phase_end_ms;
    game_tick(&g, t);
    assert(g.phase == PHASE_RESULTS);
    assert(g.winners == 0x3);  /* both players 1 and 2 */

    /* 7. Player leaves mid-round */
    printf("\n--- Test 7: player leaves mid-round ---\n");
    t = run_until_phase_changes(&g, t, 0); /* -> WAITING */
    t += TICK_MS;
    game_tick(&g, t);                   /* -> PLAYING */
    assert(g.phase == PHASE_PLAYING);
    game_remove_player(&g, 2);
    t += TICK_MS;
    game_tick(&g, t);
    assert(g.phase == PHASE_RESULTS);
    assert(g.winners == 0x1);    /* remaining player wins */

    /* 8. Gold coins appear  */
    printf("\n--- Test 8: gold coins ---\n");
    game_init(&g);
    game_add_player(&g);
    game_add_player(&g);
    int gold_seen = 0;
    for (int n = 0; n < 2000; n++)
    {
        t += TICK_MS;
        game_tick(&g, t);
        int gold_now = 0;
        for (int c = 0; c < MAX_COINS; c++)
            if (g.coins[c].active && g.coins[c].gold)
                gold_now++;
        assert(gold_now <= 1); /* never more than one gold */
        gold_seen |= gold_now;
    }
    assert(gold_seen);
    printf("Gold coin appeared, never more than one at a time\n");

    printf("\nALL TESTS PASSED\n");
    return 0;
}
