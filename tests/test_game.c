/* test_game.c 
 *
 * Build & run:
 *   gcc -Wall -Wextra -Iinclude -o build/test_game tests/test_game.c src/game.c && ./build/test_game
 *
 * Uses fake time, so a full 90-second round runs in a few milliseconds.
 */
#include "game.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void ready_all(Game *g, uint64_t t)
{
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (g->players[i].active) assert(game_continue_level(g, i + 1, t));
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
    assert(g.phase == PHASE_WAITING);
    assert(game_continue_level(&g, 1, t));
    assert(game_continue_level(&g, 1, t));
    game_tick(&g, t + 60000); assert(g.phase == PHASE_WAITING && g.ready_players == 1);
    assert(game_continue_level(&g, 2, t));
    game_tick(&g, t + TICK_MS);
    assert(g.phase == PHASE_PLAYING);
    assert(g.time_left == LEVEL_ONE_SECONDS);
    for (int c = 0; c < MAX_COINS; c++)
        assert(g.coins[c].active);


    /* Three timed levels, with scores retained at both breaks. */
    for (int level = 1; level <= 3; ++level) {
        assert(g.level == level);
        g.players[0].score = 20;
        g.players[1].score = 10;
        for (int c = 0; c < MAX_COINS; ++c) g.coins[c].active = false;
        for (int b = 0; b < MAX_BOMBS; ++b) g.bombs[b].active = false;
        t = g.phase_end_ms;
        game_tick(&g, t);
        assert(g.players[0].score == 20);
        if (level < 3) {
            assert(g.phase == PHASE_LEVEL_BREAK);
            assert(!game_continue_level(&g, 0, t));
            assert(game_continue_level(&g, 1, t)); assert(g.ready_players == 1);
            assert(game_continue_level(&g, 1, t)); assert(g.phase == PHASE_LEVEL_BREAK);
            assert(game_continue_level(&g, 2, t));
        }
    }
    assert(g.phase == PHASE_RESULTS && g.winners == 1 && g.best_score == 20);

    /* Zero lives restores the original elimination rules without losing points. */
    game_init(&g);
    for (int i = 0; i < 4; ++i) game_add_player(&g);
    t += 1000; game_tick(&g, t); ready_all(&g, t);
    t = g.phase_end_ms; game_tick(&g, t); ready_all(&g, t);
    for (int c = 0; c < MAX_COINS; ++c) g.coins[c].active = false;
    for (int b = 0; b < MAX_BOMBS; ++b) g.bombs[b].active = false;
    g.players[0].score = 8;
    for (int hit = 0; hit < 5; ++hit) {
        t += BOMB_HIT_COOLDOWN_MS;
        g.bombs[0] = (Bomb){true, g.players[0].x, PLAYER_Y, BOMB_SIZE, 0};
        game_tick(&g, t);
        assert(g.players[0].score == 8);
        assert(g.players[0].lives == (hit < 3 ? 2 - hit : 0));
        assert(g.players[0].bomb_seq == (uint32_t)(hit < 3 ? hit + 1 : 3));
        int lives_after = g.players[0].lives;
        g.bombs[0] = (Bomb){true, g.players[0].x, PLAYER_Y, BOMB_SIZE, 0};
        game_tick(&g, t + 1);
        assert(g.players[0].lives == lives_after);
        g.bombs[0].active = false;
    }
    assert(g.phase == PHASE_PLAYING && game_num_alive(&g) == 3);
    int old_x = g.players[0].x;
    g.coins[0] = (Coin){true, false, old_x, PLAYER_Y, COIN_SIZE, 0, COIN_VALUE};
    assert(game_set_input(&g, 1, 1, t)); game_tick(&g, t + TICK_MS);
    assert(g.players[0].x == old_x && g.players[0].score == 8);
    Game eliminated = g;
    eliminated.players[1].lives = eliminated.players[2].lives = 0;
    eliminated.players[3].score = 1;
    game_tick(&eliminated, t + 2 * TICK_MS);
    assert(eliminated.phase == PHASE_PLAYING && eliminated.level == 2);
    eliminated.players[3].lives = 0;
    game_tick(&eliminated,t+3*TICK_MS);
    assert(eliminated.phase == PHASE_LEVEL_BREAK && eliminated.level == 2);
    assert(eliminated.players[0].score == 8);
    assert(game_continue_level(&eliminated,1,t+2*TICK_MS));
    assert(eliminated.phase == PHASE_LEVEL_BREAK);
    ready_all(&eliminated,t+2*TICK_MS);
    assert(eliminated.phase == PHASE_PLAYING && eliminated.level == 3);
    for (int i=0;i<4;++i) assert(eliminated.players[i].lives == 3);
    assert(eliminated.players[0].score == 8);
    for (int i=0;i<3;++i) eliminated.players[i].lives=0;
    game_tick(&eliminated,t+4*TICK_MS);
    assert(eliminated.phase == PHASE_PLAYING);
    game_tick(&eliminated,eliminated.phase_end_ms);
    assert(eliminated.phase == PHASE_RESULTS && eliminated.winners == 1);
    assert(eliminated.best_score == 8);
    Game tied = eliminated;
    tied.phase = PHASE_PLAYING;
    tied.players[1].score = tied.players[0].score;
    game_tick(&tied,t+5*TICK_MS);
    assert(tied.level == 4 && tied.contenders == 3);
    assert(tied.players[0].lives == 3 && tied.players[1].lives == 3);

    g.coins[0].active = false;
    g.players[0].lives = 3;
    game_remove_player(&g, 3); game_remove_player(&g, 4);

    /* Level 3 warning lasts 1.5 seconds before a burst; gold remains unique. */
    t = g.phase_end_ms; game_tick(&g, t); assert(game_continue_level(&g, 1, t)); assert(g.ready_players == 1);
            assert(game_continue_level(&g, 1, t)); assert(g.phase == PHASE_LEVEL_BREAK);
            assert(game_continue_level(&g, 2, t));
    assert(g.level == 3);
    assert(g.players[0].lives == LIVES_PER_PLAYER);
    assert(g.players[0].score == 8);
    for (int n = 1; n <= 94; ++n) {
        t += TICK_MS; game_tick(&g, t);
        if (n < 94) assert(g.danger_lane == 0);
        int gold = 0;
        for (int c = 0; c < MAX_COINS; ++c) gold += g.coins[c].active && g.coins[c].gold;
        assert(gold <= 1);
    }
    assert(g.danger_lane == -1);
    for (int b = 0; b < MAX_BOMBS; ++b) assert(g.bombs[b].x < 200 && g.bombs[b].y > 0);

    for (int c = 0; c < MAX_COINS; ++c) g.coins[c].active = false;
    game_add_player(&g);
    int level_three_score = g.players[0].score;
    for (int hit = 1; hit <= 10; ++hit) {
        for (int b = 0; b < MAX_BOMBS; ++b) g.bombs[b].active = false;
        t += BOMB_HIT_COOLDOWN_MS;
        g.bombs[0] = (Bomb){true, g.players[0].x, PLAYER_Y, BOMB_SIZE, 0};
        game_tick(&g, t);
        assert(g.players[0].lives == (hit < 9 ? 3 - hit / 3 : 0));
        assert(g.players[0].bomb_hits == (hit < 9 ? hit % 3 : 0));
        assert(g.players[0].score == level_three_score);
    }
    assert(game_num_active(&g) == 3 && game_num_alive(&g) == 2);
    g.players[0].lives = 3;

    /* Only tied leaders enter bonus; totals persist; repeated ties replay. */
    g.players[0].score = g.players[1].score = 30; g.players[2].score = 3;
    for (int c = 0; c < MAX_COINS; ++c) g.coins[c].active = false;
    for (int b = 0; b < MAX_BOMBS; ++b) g.bombs[b].active = false;
    t = g.phase_end_ms; game_tick(&g, t);
    assert(g.level == 4 && g.phase == PHASE_PLAYING && g.contenders == 3);
    for (int b = 0; b < MAX_BOMBS; ++b) assert(!g.bombs[b].active);
    for (int c = 0; c < MAX_COINS; ++c) g.coins[c].active = false;
    old_x = g.players[2].x;
    game_set_input(&g, 3, 1, t);
    g.coins[0] = (Coin){true, false, old_x, PLAYER_Y, COIN_SIZE, 0, COIN_VALUE};
    game_tick(&g, t + TICK_MS);
    assert(g.players[2].x == old_x && g.bonus_score[2] == 0);
    g.coins[0].active = false;
    t = g.phase_end_ms; game_tick(&g, t);
    assert(g.phase == PHASE_PLAYING && g.level == 4 && g.contenders == 3);
    for (int c = 0; c < MAX_COINS; ++c) g.coins[c].active = false;
    g.coins[0] = (Coin){true, false, g.players[1].x, PLAYER_Y, COIN_SIZE, 0, COIN_VALUE};
    game_tick(&g, t + TICK_MS);
    assert(g.bonus_score[1] == 1 && g.players[1].score == 30);
    g.coins[0].active = false;
    t = g.phase_end_ms; game_tick(&g, t);
    assert(g.phase == PHASE_RESULTS && g.winners == 2 && g.best_score == 30);
    game_tick(&g, t + 60000); assert(g.phase == PHASE_RESULTS);
    assert(game_continue_level(&g, 1, t + 60000));
    assert(game_continue_level(&g, 2, t + 60000));
    assert(g.phase == PHASE_RESULTS && g.ready_players == 3);
    assert(game_continue_level(&g, 3, t + 60000));
    assert(g.phase == PHASE_PLAYING);
    t += 60000;
    game_tick(&g, t + TICK_MS); assert(g.level == 1 && g.contenders == 0 && g.players[0].score == 0);
    game_remove_player(&g, 2); game_remove_player(&g, 3);
    game_tick(&g, t + 2 * TICK_MS); assert(g.phase == PHASE_RESULTS && g.winners == 1);
    /* A disconnected voter does not block the remaining ready players. */
    game_init(&g); game_add_player(&g); game_add_player(&g); game_add_player(&g);
    game_tick(&g, t); ready_all(&g, t); t = g.phase_end_ms; game_tick(&g, t);
    assert(game_continue_level(&g, 1, t));
    assert(game_continue_level(&g, 2, t));
    assert(g.phase == PHASE_LEVEL_BREAK && g.ready_players == 3);
    assert(game_remove_player(&g, 3));
    game_tick(&g, t + TICK_MS);
    assert(g.phase == PHASE_PLAYING && g.level == 2 && g.ready_players == 0);
    /* Reused slots and new matches never inherit an old ready vote. */
    assert(game_add_player(&g) == 3 && g.ready_players == 0);
    assert(!game_continue_level(&g, 3, t));
    puts("ALL TESTS PASSED");
    return 0;
}
