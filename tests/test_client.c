
#define main coin_rush_main
#include "../src/client.c"
#undef main
#include <assert.h>
int main(void) {
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    g_screen = UI_MAIN_MENU; g_name_focused = true;
    snprintf(g_display_name, sizeof(g_display_name), "abcdef");
    key_callback(NULL, GLFW_KEY_BACKSPACE, 0, GLFW_PRESS, 0);
    for (int i=0;i<8;++i) key_callback(NULL, GLFW_KEY_BACKSPACE, 0, GLFW_REPEAT, 0);
    assert(g_display_name[0] == 0);
    snprintf(g_display_name, sizeof(g_display_name), "SelectedName");
    key_callback(NULL, GLFW_KEY_A, 0, GLFW_PRESS, GLFW_MOD_CONTROL);
    assert(g_name_selected);
    character_callback(NULL, 'Z'); assert(strcmp(g_display_name,"Z")==0 && !g_name_selected);
    key_callback(NULL, GLFW_KEY_A, 0, GLFW_PRESS, GLFW_MOD_CONTROL);
    key_callback(NULL, GLFW_KEY_DELETE, 0, GLFW_PRESS, 0); assert(g_display_name[0]==0);
    for (int i=0;i<30;++i) { character_callback(NULL, 'a'); }
    assert(strlen(g_display_name)==16);
    g_screen=UI_JOIN_CODE; snprintf(g_room_code_input,7,"123456");g_room_code_length=6;
    key_callback(NULL,GLFW_KEY_A,0,GLFW_PRESS,GLFW_MOD_CONTROL);
    character_callback(NULL,'7');assert(strcmp(g_room_code_input,"7")==0 && g_room_code_length==1);
    key_callback(NULL,GLFW_KEY_BACKSPACE,0,GLFW_REPEAT,0);assert(g_room_code_length==0);
    puts("PASS held Backspace, Ctrl+A replacement/delete, and input limits");
    initialize_audio(); assert(g_audio_ready && g_coin_sound && g_bomb_sound && g_tick_sound);
    assert(g_level_one_music && g_level_two_music && g_level_three_music && g_bonus_music);
    GameState state={0};state.phase=1;state.player_active[0]=true;
    for(int level=1;level<=4;++level) {
        state.level=level; update_game_music(&state,true);
        assert(g_music_track==level-1 && Mix_PlayingMusic());
    }
    Mix_HaltChannel(-1);
    int before=g_music_volume;
    assert(handle_sound_click(695,SOUND_CONTROL_Y+14));assert(g_music_volume==before-16 && Mix_VolumeMusic(-1)==g_music_volume*MUSIC_MIX_PERCENT/100);
    assert(Mix_PlayingMusic() && Mix_Playing(0) && Mix_GetChunk(0)==g_tick_sound);
    assert(handle_sound_click(777,SOUND_CONTROL_Y+14));assert(g_music_volume==before && Mix_Volume(-1,-1)==before);
    assert(handle_sound_click(660,SOUND_CONTROL_Y+14));assert(g_music_muted && Mix_VolumeMusic(-1)==0 && Mix_Volume(-1,-1)==0);
    assert(handle_sound_click(777,SOUND_CONTROL_Y+14));assert(!g_music_muted && Mix_VolumeMusic(-1)>0);
    assert(handle_sound_click(756,SOUND_CONTROL_Y+14));assert(g_music_volume==MIX_MAX_VOLUME);
    for(int i=0;i<10;++i)handle_sound_click(695,SOUND_CONTROL_Y+14);
    assert(g_music_volume==0 && Mix_VolumeMusic(-1)==0);
    handle_sound_click(660,SOUND_CONTROL_Y+14);assert(!g_music_muted && g_music_volume>0 && Mix_VolumeMusic(-1)>0);
    for(int i=0;i<10;++i)handle_sound_click(695,SOUND_CONTROL_Y+14);
    handle_sound_click(777,SOUND_CONTROL_Y+14);assert(g_music_volume==16 && Mix_VolumeMusic(-1)==16*MUSIC_MIX_PERCENT/100);
    audio_focus_callback(NULL,GLFW_FALSE);
    assert(Mix_VolumeMusic(-1)==0 && Mix_Volume(-1,-1)==0);
    audio_focus_callback(NULL,GLFW_TRUE);
    assert(Mix_VolumeMusic(-1)==g_music_volume*MUSIC_MIX_PERCENT/100 && Mix_Volume(-1,-1)==g_music_volume);
    /* Check mouse coordinates at 1x, 2x DPI, and with letterboxing. */
    for(int scenario=0;scenario<3;++scenario) {
        g_viewport.window_width=scenario==2?1000:800;g_viewport.window_height=600;
        g_viewport.framebuffer_width=scenario==1?1600:g_viewport.window_width;
        g_viewport.framebuffer_height=scenario==1?1200:600;
        g_viewport.width=scenario==1?1600:800;g_viewport.height=scenario==1?1200:600;
        g_viewport.x=scenario==2?100:0;g_viewport.y=0;
        double sx,sy;window_to_scene(660+(scenario==2?100:0),SOUND_CONTROL_Y+14,&sx,&sy);
        assert(fabs(sx-660)<0.1 && fabs(sy-(SOUND_CONTROL_Y+14))<0.1);
        bool muted_before=g_music_muted; assert(handle_sound_click(sx,sy));assert(g_music_muted!=muted_before);
        handle_sound_click(sx,sy);
    }
    observe_coin_collects(&state,0);
    ++state.player_collect_seq[0];observe_coin_collects(&state,1);assert(Mix_Playing(-1)>0);
    Mix_HaltChannel(-1);observe_coin_collects(&state,1);assert(Mix_Playing(-1)==0);
    ++state.player_bomb_seq[0];observe_coin_collects(&state,2);assert(Mix_Playing(-1)>0);
    Mix_HaltChannel(-1);observe_coin_collects(&state,2);assert(Mix_Playing(-1)==0);
    handle_sound_click(660,SOUND_CONTROL_Y+14);assert(g_music_muted);
    ++state.player_collect_seq[0];++state.player_bomb_seq[0];observe_coin_collects(&state,3);assert(Mix_Playing(-1)==0);
    handle_sound_click(660,SOUND_CONTROL_Y+14);Mix_HaltChannel(-1);observe_coin_collects(&state,3);assert(Mix_Playing(-1)==0);
    state.phase=0;update_game_music(&state,true);assert(!Mix_PlayingMusic());
    Mix_HaltChannel(-1);
    handle_sound_click(777,SOUND_CONTROL_Y+14);assert(Mix_Playing(-1)>0);
    assert(Mix_GetChunk(0)==g_tick_sound);
    printf("PASS audio driver %s: four music tracks, coin/bomb events, mute, +/- and volume bars\n",SDL_GetCurrentAudioDriver());
    Mix_HaltChannel(-1);Mix_HaltMusic();Mix_FreeChunk(g_coin_sound);Mix_FreeChunk(g_bomb_sound);Mix_FreeChunk(g_tick_sound);
    Mix_FreeMusic(g_level_one_music);Mix_FreeMusic(g_level_two_music);Mix_FreeMusic(g_level_three_music);Mix_FreeMusic(g_bonus_music);
    Mix_CloseAudio();Mix_Quit();SDL_Quit();return 0;
}
