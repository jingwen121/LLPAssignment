/* Coin Rush: GLFW/OpenGL client with DDS room-code matchmaking. */
#if defined(__linux__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <GLFW/glfw3.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <dds/dds.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <messages.h>
#include "network.h"
#include "stb_easy_font.h"

#define STB_EASY_FONT_IMPLEMENTATION

#define ARENA_W 800
#define ARENA_H 600
#define HUD_H 76
#define PLAYER_W 50
#define PLAYER_H 50
#define MAX_PLAYERS 5
#define MAX_COINS 8
#define MAX_BOMBS 4
#define MAX_ROOM_SAMPLES 16
#define CAT_SPRITE_SIZE 64
#define CAT_ASSET_DIR "assets/cats/idle/"
#define TOFU_ASSET_PATH "assets/menu/tofu_idle.rgba"
#define CATO_WALK_ASSET_PATH "assets/menu/cato_walk.rgba"
#define CATO_JUMP_ASSET_PATH "assets/menu/cato_jump.rgba"
#define TOFU_SHEET_WIDTH 1280
#define TOFU_SHEET_HEIGHT 1024
#define TOFU_FRAME_SIZE 256
#define TOFU_SHEET_COLUMNS 5
#define TOFU_SHEET_FRAMES 17
#define CATO_SHEET_WIDTH 1280
#define CATO_SHEET_HEIGHT 1280
#define CATO_FRAME_SIZE 256
#define CATO_SHEET_COLUMNS 5
#define CATO_WALK_FRAMES 21
#define CATO_JUMP_FRAMES 25
#define MENU_CAT_COUNT 2
#define MENU_CAT_SIZE 150.0f
#define BOMB_SHEET_PATH "assets/effects/bomb_spritesheet.rgba"
#define BOMB_SHEET_WIDTH 64
#define BOMB_SHEET_HEIGHT 16
#define BOMB_FRAME_SIZE 16
#define PLUS_SHEET_PATH "assets/effects/plus_spritesheet.rgba"
#define PLUS_SHEET_WIDTH 640
#define PLUS_SHEET_HEIGHT 1024
#define PLUS_FRAME_SIZE 128
#define HEART_SHEET_PATH "assets/effects/colored_hearts.rgba"
#define HEART_SHEET_WIDTH 116
#define HEART_SHEET_HEIGHT 85
#define LEVEL_ONE_MUSIC_PATH "assets/music/level1.mp3"
#define LEVEL_TWO_MUSIC_PATH "assets/music/level2.mp3"
#define LEVEL_THREE_MUSIC_PATH "assets/music/level3.mp3"
#define BONUS_MUSIC_PATH "assets/music/bonus.mp3"

typedef enum { UI_MAIN_MENU, UI_JOIN_CODE, UI_CONNECTING, UI_GAME, UI_EXIT_CONFIRM } UiScreen;

typedef struct
{
    int player_id;
    char direction;
    bool running;
    bool join_requested;
    bool continue_requested;
    bool create_room;
    bool join_failed;
    uint32_t room_code;
    uint64_t client_token;
    uint64_t join_started;
    GameState state;
    char display_name[17];
    bool has_state;
    bool network_failed;
} ClientData;

static ClientData g_client;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_player_facing[MAX_PLAYERS] = {1, 1, 1, 1, 1};
static UiScreen g_screen = UI_MAIN_MENU;
static char g_room_code_input[7] = "";
static size_t g_room_code_length;
static char g_ui_error[80] = "";
static char g_display_name[17] = "";
static bool g_name_focused = true;
static bool g_name_selected;
static bool g_code_selected;
static bool g_mouse_clicked;
static double g_click_x, g_click_y;
static bool g_exit_from_game = false;
typedef struct
{
    int x, y, width, height;
    int framebuffer_width, framebuffer_height;
    int window_width, window_height;
} SceneViewport;
static SceneViewport g_viewport;

typedef enum { MENU_CAT_IDLE, MENU_CAT_WALK, MENU_CAT_JUMP } MenuCatAnimation;
typedef struct
{
    float x, base_y, min_x, max_x, size, speed;
    float animation_start_x;
    int facing, frame_offset;
    int animation_start_facing;
    MenuCatAnimation animation;
    double animation_started, next_animation;
} MenuCat;

typedef struct
{
    bool tracked;
    bool was_active;
    bool active;
    uint32_t last_collect_seq;
    double started;
    float x, y;
} PickupEffect;

static MenuCat g_menu_cats[MENU_CAT_COUNT];
static GLuint g_tofu_texture;
static GLuint g_cato_walk_texture;
static GLuint g_cato_jump_texture;
static GLuint g_bomb_texture;
static GLuint g_plus_texture;
static GLuint g_heart_texture;
static PickupEffect g_pickup_effects[MAX_PLAYERS];
static Mix_Music *g_level_one_music;
static Mix_Music *g_level_two_music;
static Mix_Music *g_level_three_music;
static Mix_Music *g_bonus_music;
static Mix_Chunk *g_coin_sound, *g_bomb_sound, *g_tick_sound;
static uint32_t g_last_bomb_seq[MAX_PLAYERS];
static bool g_audio_ready;
static bool g_audio_focused = true;
#define MUSIC_MIX_PERCENT 30
#define SOUND_CONTROL_Y 32
static bool g_music_muted;
static int g_music_volume = MIX_MAX_VOLUME * 3 / 5;
static int g_music_track = -1;

static void glfw_error_callback(int code, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, description ? description : "unknown error");
}

static void apply_music_volume(void)
{
    if (g_audio_ready)
        Mix_VolumeMusic(g_music_muted ? 0 : g_music_volume);
}

static void audio_focus_callback(GLFWwindow *window, int focused)
{
    (void)window;
    g_audio_focused = focused == GLFW_TRUE;
    apply_music_volume();
    if (!g_audio_focused && g_audio_ready) Mix_HaltChannel(-1);
}

static void update_game_music(const GameState *state, bool has_state)
{
    if (!g_audio_ready)
        return;

    int track = !has_state || state->phase != 1 ? -1 :
                (state->level >= 2 ? 1 : 0);
    if (track == g_music_track)
        return;

    Mix_HaltMusic();
    g_music_track = track;
    Mix_Music *music = track == 0 ? g_level_one_music :
                       track == 1 ? g_level_two_music : NULL;
    if (music && Mix_PlayMusic(music, -1) != 0)
        fprintf(stderr, "Could not play level music: %s\n", Mix_GetError());
    apply_music_volume();
}

static void initialize_audio(void)
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0)
    {
        fprintf(stderr, "Audio unavailable; continuing without music: %s\n", SDL_GetError());
        return;
    }
    Mix_Init(MIX_INIT_MP3);
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) != 0)
    {
        fprintf(stderr, "Audio unavailable; continuing without music: %s\n", Mix_GetError());
        Mix_Quit();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }

    g_audio_ready = true;
    g_level_one_music = Mix_LoadMUS(LEVEL_ONE_MUSIC_PATH);
    if (!g_level_one_music)
        fprintf(stderr, "Could not load Level 1 music (%s): %s\n",
                LEVEL_ONE_MUSIC_PATH, Mix_GetError());
    g_level_two_music = Mix_LoadMUS(LEVEL_TWO_MUSIC_PATH);
    if (!g_level_two_music)
        fprintf(stderr, "Could not load Level 2 music (%s): %s\n",
                LEVEL_TWO_MUSIC_PATH, Mix_GetError());
    apply_music_volume();
}

static void update_viewport(GLFWwindow *window, int framebuffer_width, int framebuffer_height)
{
    if (framebuffer_width <= 0 || framebuffer_height <= 0)
        return;
    g_viewport.framebuffer_width = framebuffer_width;
    g_viewport.framebuffer_height = framebuffer_height;
    glfwGetWindowSize(window, &g_viewport.window_width, &g_viewport.window_height);
    if (g_viewport.window_width <= 0 || g_viewport.window_height <= 0)
        return;

    float scale = fminf((float)framebuffer_width / (float)ARENA_W,
                        (float)framebuffer_height / (float)ARENA_H);
    g_viewport.width = (int)lroundf((float)ARENA_W * scale);
    g_viewport.height = (int)lroundf((float)ARENA_H * scale);
    g_viewport.x = (framebuffer_width - g_viewport.width) / 2;
    g_viewport.y = (framebuffer_height - g_viewport.height) / 2;
    glViewport(g_viewport.x, g_viewport.y, g_viewport.width, g_viewport.height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, ARENA_W, ARENA_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    update_viewport(window, width, height);
}

static void window_to_scene(double window_x, double window_y, double *scene_x, double *scene_y)
{
    if (g_viewport.window_width <= 0 || g_viewport.window_height <= 0 ||
        g_viewport.width <= 0 || g_viewport.height <= 0)
    {
        *scene_x = *scene_y = -10000.0;
        return;
    }
    double framebuffer_x = window_x * (double)g_viewport.framebuffer_width / g_viewport.window_width;
    double framebuffer_y = window_y * (double)g_viewport.framebuffer_height / g_viewport.window_height;
    double viewport_top = g_viewport.framebuffer_height - (g_viewport.y + g_viewport.height);
    if (framebuffer_x < g_viewport.x || framebuffer_x >= g_viewport.x + g_viewport.width ||
        framebuffer_y < viewport_top || framebuffer_y >= viewport_top + g_viewport.height)
    {
        *scene_x = *scene_y = -10000.0;
        return;
    }
    *scene_x = (framebuffer_x - g_viewport.x) * ARENA_W / g_viewport.width;
    *scene_y = (framebuffer_y - viewport_top) * ARENA_H / g_viewport.height;
}

static bool configure_wslg_renderer(void)
{
    bool software_fallback = false;
#if defined(__linux__)
    const char *wsl = getenv("WSL_DISTRO_NAME");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *software_gl = getenv("LIBGL_ALWAYS_SOFTWARE");
    if (wsl && *wsl && wayland && *wayland)
    {
        if (!software_gl || !*software_gl)
            software_fallback = setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1) == 0;
        else
            software_fallback = strcmp(software_gl, "1") == 0;
        if (software_fallback && !getenv("LP_NUM_THREADS"))
            setenv("LP_NUM_THREADS", "2", 0);
        if (software_fallback)
            fprintf(stderr, "WSLg detected; using Mesa software OpenGL (2 worker threads).\n");
    }
#endif
    return software_fallback;
}

static void draw_rect(float x, float y, float w, float h)
{
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}

static void draw_text(float x, float y, float scale, const char *text)
{
    char vertices[65536];
    int quads = stb_easy_font_print(0, 0, (char *)text, NULL, vertices, sizeof(vertices));
    if (quads <= 0)
        return;
    glPushMatrix();
    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 16, vertices);
    glDrawArrays(GL_QUADS, 0, quads * 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glPopMatrix();
}

// Measure the font itself so short and long labels share the same centre
static void draw_centered(float centre, float y, float scale, const char *text)
{
    struct { float x, y, z; unsigned char color[4]; } vertices[4096];
    int quads = stb_easy_font_print(0, 0, (char *)text, NULL, vertices, sizeof(vertices));
    if (quads <= 0) return;
    float left = vertices[0].x, right = left;
    for (int i = 1; i < quads * 4; ++i) {
        if (vertices[i].x < left) left = vertices[i].x;
        if (vertices[i].x > right) right = vertices[i].x;
    }
    draw_text(centre - (left + right) * scale * 0.5f, y, scale, text);
}

static void text_vertical_bounds(const char *text, float *top, float *bottom)
{
    struct { float x, y, z; unsigned char color[4]; } vertices[4096];
    int quads = stb_easy_font_print(0, 0, (char *)text, NULL, vertices, sizeof(vertices));
    *top = *bottom = 0;
    if (quads <= 0) return;
    *top = *bottom = vertices[0].y;
    for (int i = 1; i < quads * 4; ++i) {
        if (vertices[i].y < *top) *top = vertices[i].y;
        if (vertices[i].y > *bottom) *bottom = vertices[i].y;
    }
}

static float text_y_at_center(const char *text, float scale, float center)
{
    float top, bottom; text_vertical_bounds(text, &top, &bottom);
    return center - (top + bottom) * scale * 0.5f;
}

static float menu_origin_y(void)
{
    float top, bottom;
    text_vertical_bounds("COIN RUSH", &top, &bottom);
    float title_height = (bottom - top) * 3.2f;
    text_vertical_bounds("UP TO FIVE PLAYERS  |  LEFT / RIGHT TO MOVE", &top, &bottom);
    float footer_height = bottom - top;
    return ARENA_H * 0.5f - (350.0f + (footer_height - title_height) * 0.5f) * 0.5f;
}

static int window_x, window_y, window_w = ARENA_W, window_h = ARENA_H;

static void toggle_fullscreen(GLFWwindow *window)
{
    if (glfwGetWindowMonitor(window))
        glfwSetWindowMonitor(window, NULL, window_x, window_y, window_w, window_h, GLFW_DONT_CARE);
    else
    {
        // Save the window before switching monitors; restore its position on exit
        glfwGetWindowPos(window, &window_x, &window_y);
        glfwGetWindowSize(window, &window_w, &window_h);
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode) glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    }
}

static void draw_coin(int x, int y, int size, bool gold)
{
    const int segments = 20;
    float radius = size * 0.5f;
    float cx = x + radius, cy = y + radius;
    glColor3f(gold ? 1.0f : 0.33f, gold ? 0.78f : 0.91f, gold ? 0.19f : 0.72f);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segments; ++i)
    {
        float a = (float)i * 6.28318530718f / (float)segments;
        glVertex2f(cx + radius * cosf(a), cy + radius * sinf(a));
    }
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < segments; ++i)
    {
        float a = (float)i * 6.28318530718f / (float)segments;
        glVertex2f(cx + radius * 0.68f * cosf(a), cy + radius * 0.68f * sinf(a));
    }
    glEnd();
}

static GLuint load_rgba_texture(const char *path, int width, int height)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Sprite missing: %s\n", path);
        return 0;
    }
    size_t expected = (size_t)width * (size_t)height * 4u;
    unsigned char *pixels = malloc(expected);
    if (!pixels)
    { fclose(file); return 0; }
    size_t bytes = fread(pixels, 1, expected, file);
    int extra = fgetc(file);
    fclose(file);
    if (bytes != expected || extra != EOF)
    {
        fprintf(stderr, "Invalid sprite data: %s\n", path);
        free(pixels);
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

static GLuint load_cat_texture(const char *direction)
{
    char path[128];
    snprintf(path, sizeof(path), CAT_ASSET_DIR "%s.rgba", direction);
    return load_rgba_texture(path, CAT_SPRITE_SIZE, CAT_SPRITE_SIZE);
}

static void draw_atlas_frame(GLuint texture, int frame, int frame_count,
                             int columns, int rows, int cell_size,
                             int sheet_width, int sheet_height,
                             float x, float y, float size, bool flip_x)
{
    if (!texture || frame < 0 || frame >= frame_count || columns <= 0 ||
        rows <= 0 || frame / columns >= rows)
        return;
    int column = frame % columns, row = frame / columns;
    float u0 = (float)(column * cell_size) / (float)sheet_width;
    float u1 = (float)((column + 1) * cell_size) / (float)sheet_width;
    float v0 = (float)(row * cell_size) / (float)sheet_height;
    float v1 = (float)((row + 1) * cell_size) / (float)sheet_height;
    if (flip_x)
    { float temp = u0; u0 = u1; u1 = temp; }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + size, y);
    glTexCoord2f(u1, v1); glVertex2f(x + size, y + size);
    glTexCoord2f(u0, v1); glVertex2f(x, y + size);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

static void draw_bomb(const GameState *state, int i, double now)
{
    if (!g_bomb_texture || !state->bomb_active[i])
        return;
    int frame = ((int)(now * 8.0) + i) % 4;
    draw_atlas_frame(g_bomb_texture, frame, 4, 4, 1, BOMB_FRAME_SIZE,
                     BOMB_SHEET_WIDTH, BOMB_SHEET_HEIGHT,
                     (float)state->bomb_x[i] - 4.0f,
                     (float)state->bomb_y[i] - 4.0f, 32.0f, false);
}

static void observe_coin_collects(const GameState *state, double now)
{
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        PickupEffect *effect = &g_pickup_effects[i];
        bool active = state->player_active[i];
        if (!effect->tracked || active != effect->was_active)
        {
            effect->tracked = true;
            effect->was_active = active;
            effect->last_collect_seq = state->player_collect_seq[i];
            g_last_bomb_seq[i] = state->player_bomb_seq[i];
            continue;
        }
        if (active && state->player_collect_seq[i] != effect->last_collect_seq)
        {
            effect->last_collect_seq = state->player_collect_seq[i];
            effect->started = now;
            effect->x = (float)state->player_x[i] + (float)PLAYER_W * 0.5f;
            effect->y = (float)state->player_y[i] - 80.0f;
            effect->active = true;
            if (g_audio_ready && g_coin_sound && g_audio_focused && !g_music_muted && g_music_volume > 0)
                Mix_PlayChannel(-1, g_coin_sound, 0);
        }
        if (active && state->player_bomb_seq[i] != g_last_bomb_seq[i]) {
            g_last_bomb_seq[i] = state->player_bomb_seq[i];
            if (g_audio_ready && g_bomb_sound && g_audio_focused && !g_music_muted && g_music_volume > 0)
                Mix_PlayChannel(-1, g_bomb_sound, 0);
        }
    }
}

static void draw_coin_collect_fx(double now)
{
    if (!g_plus_texture)
        return;
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        PickupEffect *effect = &g_pickup_effects[i];
        if (!effect->active)
            continue;
        double elapsed = now - effect->started;
        if (elapsed >= 0.72)
        {
            effect->active = false;
            continue;
        }
        int frame = ((int)(elapsed * 32.0)) % 40;
        float size = 36.0f;
        float y = effect->y - (float)elapsed * 26.0f;
        draw_atlas_frame(g_plus_texture, frame, 40, 5, 8, PLUS_FRAME_SIZE,
                         PLUS_SHEET_WIDTH, PLUS_SHEET_HEIGHT,
                         effect->x - size * 0.5f, y, size, false);
    }
}

static double random_between(double low, double high)
{
    return low + (high - low) * ((double)rand() / (double)RAND_MAX);
}

static void choose_menu_cat_animation(MenuCat *cat, double now)
{
    int choice = rand() % 100;
    cat->animation = choice < 42 ? MENU_CAT_IDLE :
                     choice < 78 ? MENU_CAT_WALK : MENU_CAT_JUMP;
    cat->animation_started = now;
    cat->animation_start_x = cat->x;
    cat->animation_start_facing = cat->facing;
    cat->frame_offset = rand() % TOFU_SHEET_FRAMES;
    double duration = cat->animation == MENU_CAT_IDLE ? random_between(1.0, 2.8) :
                      cat->animation == MENU_CAT_WALK ? random_between(1.4, 2.7) :
                                                        random_between(1.25, 1.65);
    cat->next_animation = now + duration;
}

static void initialize_menu_cats(double now)
{
    for (int i = 0; i < MENU_CAT_COUNT; ++i)
    {
        MenuCat *cat = &g_menu_cats[i];
        cat->size = MENU_CAT_SIZE;
        cat->speed = (float)random_between(24.0, 42.0);
        cat->min_x = i == 0 ? 8.0f : 585.0f;
        cat->max_x = i == 0 ? 220.0f - cat->size - 8.0f : (float)ARENA_W - cat->size;
        cat->x = (float)random_between(cat->min_x, cat->max_x);
        cat->base_y = (float)random_between(190.0, (double)ARENA_H - cat->size - 14.0);
        cat->facing = (rand() & 1) ? 1 : -1;
        cat->animation_start_x = cat->x;
        cat->animation_start_facing = cat->facing;
        cat->frame_offset = rand() % TOFU_SHEET_FRAMES;
        cat->animation_started = now - random_between(0.0, 2.0);
        cat->next_animation = now + random_between(0.2, 1.2);
        cat->animation = (MenuCatAnimation)(rand() % 3);
    }
}

static void update_and_draw_menu_cat(MenuCat *cat, double now)
{
    if (now >= cat->next_animation)
        choose_menu_cat_animation(cat, now);

    double elapsed = now - cat->animation_started;
    float draw_y = cat->base_y;
    GLuint texture = g_tofu_texture;
    int frame = 0, frame_count = TOFU_SHEET_FRAMES;
    int columns = TOFU_SHEET_COLUMNS, rows = 4, cell_size = TOFU_FRAME_SIZE;
    int sheet_width = TOFU_SHEET_WIDTH, sheet_height = TOFU_SHEET_HEIGHT;
    bool flip_x = false;

    if (cat->animation == MENU_CAT_IDLE)
    {
        frame = ((int)(now * 8.0) + cat->frame_offset) % TOFU_SHEET_FRAMES;
        draw_y += 2.0f * sinf((float)(elapsed * 3.0));
    }
    else if (cat->animation == MENU_CAT_WALK)
    {
        texture = g_cato_walk_texture;
        frame_count = CATO_WALK_FRAMES; rows = 5;
        sheet_width = CATO_SHEET_WIDTH; sheet_height = CATO_SHEET_HEIGHT;
        columns = CATO_SHEET_COLUMNS; cell_size = CATO_FRAME_SIZE;
        frame = ((int)(elapsed * 12.0) + cat->frame_offset) % CATO_WALK_FRAMES;
        flip_x = cat->facing < 0;
    }
    else
    {
        texture = g_cato_jump_texture;
        frame_count = CATO_JUMP_FRAMES; rows = 5;
        sheet_width = CATO_SHEET_WIDTH; sheet_height = CATO_SHEET_HEIGHT;
        columns = CATO_SHEET_COLUMNS; cell_size = CATO_FRAME_SIZE;
        float progress = (float)(elapsed / (cat->next_animation - cat->animation_started));
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        frame = (int)(progress * (float)(CATO_JUMP_FRAMES - 1));
        draw_y -= 46.0f * sinf(progress * 3.14159265f);
        flip_x = cat->facing < 0;
    }

    if (cat->animation == MENU_CAT_WALK || cat->animation == MENU_CAT_JUMP)
    {
        float motion = cat->animation == MENU_CAT_WALK ? 1.0f : 0.35f;
        /* Position is derived from the animation clock, so frame pacing cannot
         * accumulate movement error. Bounce inside a lane that stays clear of
         * the center panel and always keeps the entire sprite on screen. */
        float travel = (float)elapsed * cat->speed * motion;
        float span = cat->max_x - cat->min_x;
        if (span > 0.0f)
        {
            float period = 2.0f * span;
            float path = cat->animation_start_x - cat->min_x +
                         (float)cat->animation_start_facing * travel;
            float pos = fmodf(path, period);
            if (pos < 0.0f) pos += period;
            if (pos <= span)
            {
                cat->x = cat->min_x + pos;
                cat->facing = cat->animation_start_facing > 0 ? 1 : -1;
            }
            else
            {
                cat->x = cat->min_x + period - pos;
                cat->facing = cat->animation_start_facing > 0 ? -1 : 1;
            }
            flip_x = cat->facing < 0;
        }
    }

    /* Clamp both axes even if a frame is delayed or the window is resized. */
    if (cat->x < 0.0f) cat->x = 0.0f;
    if (cat->x + cat->size > ARENA_W) cat->x = (float)ARENA_W - cat->size;
    if (draw_y < 0.0f) draw_y = 0.0f;
    if (draw_y + cat->size > ARENA_H) draw_y = (float)ARENA_H - cat->size;
    draw_atlas_frame(texture, frame, frame_count, columns, rows, cell_size,
                     sheet_width, sheet_height, cat->x, draw_y, cat->size, flip_x);
}

static void draw_cat(GLuint texture, float x, float y, const float *tint)
{
    if (!texture)
        return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(tint[0], tint[1], tint[2], 1.0f);
    glBegin(GL_QUADS);
    /* The raw files are top-row first. v=0 addresses that uploaded row. */
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + CAT_SPRITE_SIZE, y);
    glTexCoord2f(1, 1); glVertex2f(x + CAT_SPRITE_SIZE, y + CAT_SPRITE_SIZE);
    glTexCoord2f(0, 1); glVertex2f(x, y + CAT_SPRITE_SIZE);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

static const char *player_name(const GameState *state, int i)
{
    static const char *fallback[5] = {"Player 1", "Player 2", "Player 3", "Player 4", "Player 5"};
    return state->player_names[i][0] ? state->player_names[i] : fallback[i];
}

static void draw_player(const GameState *state, int i, int my_id, int facing,
                        GLuint cat_east, GLuint cat_west)
{
    static const float tints[MAX_PLAYERS][3] = {
        {0.62f, 0.88f, 1.00f}, {1.00f, 0.58f, 0.72f}, {0.78f, 0.65f, 1.00f},
        {1.00f, 0.84f, 0.48f}, {0.55f, 1.00f, 0.67f}};
    if (!state->player_active[i])
        return;
    int x = state->player_x[i], y = state->player_y[i];
    const float eliminated_tint[3] = {0.30f, 0.32f, 0.38f};
    const float *tint = state->level == 4 && !(state->contenders & (1 << i)) ? eliminated_tint : tints[i];
    GLuint sprite = facing < 0 ? cat_west : cat_east;
    if (sprite)
        draw_cat(sprite, (float)x - 7.0f, (float)y - 28.0f, tint);
    else
    {
        glColor3f(tint[0], tint[1], tint[2]);
        draw_rect((float)x, (float)y, PLAYER_W, PLAYER_H);
        const char *label = player_name(state, i);
        glColor3f(0.04f, 0.07f, 0.12f);
        draw_centered((float)x + 25.0f, (float)y + 20.0f, 0.8f, label);
    }
    glColor3f(tint[0], tint[1], tint[2]);
    draw_centered(fmaxf(65, fminf(735, (float)x + 25)), (float)y - 48, 1.0f, player_name(state, i));
    if (my_id == i + 1)
    {
        glColor3f(0.68f, 0.91f, 1.0f);
        draw_centered((float)x + 25.0f, (float)y - 68.0f, 1.1f, "YOU");
    }
}

static const char *phase_name(int phase)
{
    if (phase == 1) return "COIN RUSH";
    if (phase == 2) return "LEVEL BREAK";
    if (phase == 3) return "ROUND COMPLETE";
    return "ROOM LOBBY";
}

static void draw_button(float x, float y, float w, float h, const char *label, bool hovered);
static void draw_sound_controls(void);

static float playfield_panel_top(float height)
{
    return HUD_H + (ARENA_H - HUD_H - height) * 0.5f;
}

static float results_panel_top(const GameState *state)
{
    int players = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) players += state->player_active[i] != 0;
    return playfield_panel_top(255.0f + 30.0f * players);
}

static float ready_button_y(const GameState *state)
{
    return state->phase == 3 ? ARENA_H + HUD_H - results_panel_top(state) - 90.0f :
        playfield_panel_top(290) + (state->phase == 0 ? 178 : 150);
}

static void draw_ready_button(const GameState *state, int player_id, float y, const char *label)
{
    int total = 0, ready = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (state->player_active[i]) {
            ++total;
            if (state->ready_players & (1 << i)) ++ready;
        }
    bool mine = player_id >= 1 && player_id <= MAX_PLAYERS &&
                (state->ready_players & (1 << (player_id - 1)));
    draw_button(264, y, 272, 48, mine ? "READY - WAITING FOR OTHERS" : label, mine);
    if (mine) {
        glColor3f(0.28f, 0.95f, 0.48f);
        glLineWidth(3);
        glBegin(GL_LINES);
        glVertex2f(278, y + 23); glVertex2f(283, y + 29);
        glVertex2f(283, y + 29); glVertex2f(292, y + 16);
        glEnd(); glLineWidth(1);
    }
    char count[48];
    snprintf(count, sizeof(count), "%d/%d READY", ready, total);
    glColor3f(mine ? 0.28f : 0.67f, mine ? 0.95f : 0.77f, mine ? 0.48f : 0.89f);
    draw_centered(400, y + 60, 1.25f, count);
}

static void draw_arena(const GameState *state, bool has_state, int player_id,
                       uint32_t room_code, const int facing[MAX_PLAYERS],
                       GLuint cat_east, GLuint cat_west, double now)
{
    glClearColor(0.035f, 0.055f, 0.095f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0.07f, 0.10f, 0.16f); draw_rect(0, 0, ARENA_W, HUD_H);
    glColor3f(0.13f, 0.19f, 0.28f); draw_rect(0, HUD_H - 2.0f, ARENA_W, 2.0f);
    glColor3f(0.10f, 0.14f, 0.21f);
    glBegin(GL_LINES);
    for (int x = 20; x < ARENA_W; x += 40)
    { glVertex2f((float)x, (float)HUD_H); glVertex2f((float)x, (float)ARENA_H); }
    for (int y = HUD_H + 20; y < ARENA_H; y += 40)
    { glVertex2f(0, (float)y); glVertex2f((float)ARENA_W, (float)y); }
    glEnd();
    glColor3f(0.43f, 0.88f, 0.92f); draw_text(24, 20, 2.1f, "COIN RUSH");
    char room_label[32];
    snprintf(room_label, sizeof(room_label), "ROOM  %06u", room_code);
    glColor3f(0.62f, 0.72f, 0.84f);
    draw_text(24, 49, 1.0f, room_label);

    if (!has_state)
    {
        glColor3f(0.90f, 0.94f, 1.0f); draw_text(24, 110, 2.0f, "CONNECTING TO ROOM...");
        return;
    }

    int active_count = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        active_count += state->player_active[i] ? 1 : 0;
    char text[96];
    char level_label[24];
    if (state->level == 4) snprintf(level_label, sizeof(level_label), "BONUS");
    else snprintf(level_label, sizeof(level_label), "LEVEL %d", state->level > 0 ? state->level : 1);
    glColor3f(0.58f, 0.83f, 1.0f); draw_centered(215, 20, 1.35f, level_label);
    glColor3f(0.88f, 0.93f, 1.0f); draw_centered(330, 20, 1.6f, phase_name(state->phase));
    glColor3f(1.0f, 0.83f, 0.42f);
    snprintf(text, sizeof(text), "TIME  %02d", state->time_left); draw_centered(525, 20, 1.8f, text);
    glColor3f(0.73f, 0.81f, 0.91f);
    int shown_capacity = MAX_PLAYERS;
    snprintf(text, sizeof(text), "PLAYERS  %d/%d", active_count, shown_capacity);
    draw_centered(714, text_y_at_center(text, 1.1f, 20), 1.1f, text);
    draw_sound_controls();

    if (state->phase == 1 && g_viewport.height > 0)
    {
        int playfield_height = (int)lroundf((float)g_viewport.height *
                                            (float)(ARENA_H - HUD_H) / (float)ARENA_H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(g_viewport.x, g_viewport.y, g_viewport.width, playfield_height);
        for (int i = 0; i < MAX_COINS; ++i)
            if (state->coin_active[i])
                draw_coin(state->coin_x[i], state->coin_y[i], state->coin_size[i], state->coin_gold[i]);
        for (int i = 0; i < MAX_BOMBS; ++i)
            if (state->level >= 2)
                draw_bomb(state, i, now);
        glDisable(GL_SCISSOR_TEST);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        draw_player(state, i, player_id, facing[i], cat_east, cat_west);

    }
    if (state->phase == 1)
        draw_coin_collect_fx(now);
    if (state->phase == 1 && state->level == 3 && state->danger_lane >= 0) {
        glColor3f(0.95f, 0.15f, 0.12f);
        draw_rect((float)state->danger_lane * 200, 590, 200, 10);
        draw_centered(400, 100, 1.2f, "DANGER: MOVE OFF THE RED FLOOR!");
    }
    if (state->phase == 1 && state->level == 4) {
        glColor3f(1, 0.83f, 0.42f);
        draw_centered(400, 100, 1.2f, "TIE BREAK: 15 SECOND COIN RUSH");
        snprintf(text, sizeof(text), "YOUR BONUS: %d%s", state->bonus_score[player_id > 0 ? player_id - 1 : 0],
                 player_id > 0 && (state->contenders & (1 << (player_id - 1))) ? "" : "  SPECTATING");
        draw_centered(400, 125, 1.2f, text);
    }


    if (state->phase == 0)
    {
        float top = playfield_panel_top(290);
        glColor3f(0.05f, 0.08f, 0.13f); draw_rect(170, top, 460, 290);
        glColor3f(0.16f, 0.24f, 0.34f); draw_rect(170, top, 460, 3);
        glColor3f(0.92f, 0.96f, 1); draw_centered(ARENA_W * 0.5f, top + 29, 2.1f, "ROOM LOBBY");
        glColor3f(0.67f, 0.77f, 0.89f);
        snprintf(text, sizeof(text), "%d PLAYERS - AT LEAST 2 NEEDED", active_count);
        draw_centered(ARENA_W * 0.5f, top + 72, 1.25f, text);
        glColor3f(1, 0.83f, 0.42f);
        snprintf(text, sizeof(text), "ROOM CODE  %06u", room_code); draw_centered(ARENA_W * 0.5f, top + 111, 1.65f, text);
        glColor3f(0.57f, 0.68f, 0.81f);
        draw_centered(ARENA_W * 0.5f, top + 152, 1.05f, "Share this code, then everyone presses READY");
        draw_ready_button(state, player_id, ready_button_y(state), "READY TO START");
    }
    else if (state->phase == 2)
    {
        float top = playfield_panel_top(290);
        glColor3f(0.04f, 0.07f, 0.12f); draw_rect(150, top, 500, 290);
        glColor3f(0.27f, 0.80f, 0.86f); draw_rect(150, top, 500, 3);
        glColor3f(0.43f, 0.88f, 0.92f);
        snprintf(text, sizeof(text), "LEVEL %d COMPLETE", state->level);
        draw_centered(ARENA_W * 0.5f, top + 38, 2.2f, text);
        glColor3f(0.88f, 0.93f, 1.0f);
        draw_centered(ARENA_W * 0.5f, top + 80, 1.25f, "Your score carries into the next level");
        glColor3f(1.0f, 0.83f, 0.42f);
        draw_centered(ARENA_W * 0.5f, top + 114, 1.2f, state->level == 1 ? "Bombs cost 3 points. Everyone keeps playing!" : "Watch the red floor: bomb bursts are coming!");
        snprintf(text, sizeof(text), "CONTINUE TO LEVEL %d", state->level + 1);
        draw_ready_button(state, player_id, ready_button_y(state), text);
        glColor3f(0.57f, 0.68f, 0.81f);
        draw_centered(ARENA_W * 0.5f, top + 230, 1.05f, "Everyone must press ENTER or click to continue");
    }
    else if (state->phase == 3)
    {
        float top = results_panel_top(state);
        glColor3f(0.04f, 0.07f, 0.12f); draw_rect(150, top, 500, ARENA_H + HUD_H - 2 * top);
        glColor3f(0.99f, 0.73f, 0.27f); draw_rect(150, top, 500, 3);
        glColor3f(0.99f, 0.83f, 0.39f); draw_centered(ARENA_W * 0.5f, top + 24, 2.2f, "ROUND COMPLETE");
        char winners[96] = ""; size_t used = 0;
        for (int i = 0; i < MAX_PLAYERS; ++i)
            if ((state->winners & (1 << i)) != 0)
            {
                int n = snprintf(winners + used, sizeof(winners) - used,
                                 "%s%s", used ? " & " : "", player_name(state, i));
                if (n > 0 && (size_t)n < sizeof(winners) - used) used += (size_t)n;
            }
        if (!used) snprintf(winners, sizeof(winners), "No winner");
        glColor3f(0.91f, 0.95f, 1); draw_centered(ARENA_W * 0.5f, top + 60, 1.4f, "WINNER");
        draw_centered(ARENA_W * 0.5f, top + 87, 1.8f, winners);
        glColor3f(0.67f, 0.77f, 0.89f);
        snprintf(text, sizeof(text), "TOP SCORE  %d PTS", state->best_score);
        draw_centered(ARENA_W * 0.5f, top + 118, 1.4f, text);
        glColor3f(0.45f, 0.58f, 0.73f);
        draw_ready_button(state, player_id, ready_button_y(state), "CONTINUE TO NEXT ROUND");
        glColor3f(0.67f, 0.77f, 0.89f);
        float name_width = 0, score_width = 0;
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            if (!state->player_active[i]) continue;
            name_width = fmaxf(name_width, stb_easy_font_width((char *)player_name(state, i)) * 1.3f);
            snprintf(text, sizeof(text), "%d PTS", state->player_score[i]);
            score_width = fmaxf(score_width, stb_easy_font_width(text) * 1.3f);
        }
        float rank_width = stb_easy_font_width("5.") * 1.3f;
        float table_width = rank_width + 12 + name_width + 64 + score_width;
        float rank_x = (ARENA_W - table_width) * 0.5f;
        float name_x = rank_x + rank_width + 12;
        float score_right = name_x + name_width + 64 + score_width;
        int row = 0;
        for (int order = 0; order < MAX_PLAYERS; ++order)
            for (int i = 0; i < MAX_PLAYERS; ++i) {
                if (!state->player_active[i]) continue;
                int ahead = 0, rank = 1;
                for (int j = 0; j < MAX_PLAYERS; ++j) {
                    if (!state->player_active[j]) continue;
                    bool greater = state->player_score[j] > state->player_score[i] ||
                        (state->player_score[j] == state->player_score[i] && state->level == 4 &&
                         state->bonus_score[j] > state->bonus_score[i]);
                    if (greater) ++rank;
                    if (greater || (state->player_score[j] == state->player_score[i] &&
                        (state->level != 4 || state->bonus_score[j] == state->bonus_score[i]) && j < i)) ++ahead;
                }
                if (ahead != order) continue;
                float row_y = top + 155 + row++ * 30;
                snprintf(text, sizeof(text), "%d.", rank);
                draw_text(rank_x, row_y, 1.3f, text);
                draw_text(name_x, row_y, 1.3f, player_name(state, i));
                snprintf(text, sizeof(text), "%d PTS", state->player_score[i]);
                draw_text(score_right - stb_easy_font_width(text) * 1.3f, row_y, 1.3f, text);
                if (state->winners & (1 << i)) draw_text(score_right + 18, row_y, 1.3f, "WINNER");
            }
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (state->player_active[i])
        {
            snprintf(text, sizeof(text), "%s  %d pts%s", player_name(state, i), state->player_score[i],
                     player_id == i + 1 ? "  YOU" : "");
            glColor3f(player_id == i + 1 ? 0.61f : 0.75f,
                      player_id == i + 1 ? 0.89f : 0.82f, 0.97f);
            draw_centered(((float)i + 0.5f) * ARENA_W / MAX_PLAYERS, (float)ARENA_H - 22.0f, 0.85f, text);
        }
}

static bool point_in_rect(double x, double y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

static void draw_button(float x, float y, float w, float h, const char *label, bool hovered)
{
    glColor3f(hovered ? 0.13f : 0.09f, hovered ? 0.25f : 0.17f, hovered ? 0.34f : 0.25f);
    draw_rect(x, y, w, h);
    glColor3f(hovered ? 0.36f : 0.23f, hovered ? 0.86f : 0.65f, hovered ? 0.88f : 0.75f);
    draw_rect(x, y, w, 2.0f);
    glColor3f(0.91f, 0.96f, 1.0f);
    // Match both axes to the actual font dimensions, including scaled labels
    struct { float x, y, z; unsigned char color[4]; } vertices[4096];
    int quads = stb_easy_font_print(0, 0, (char *)label, NULL, vertices, sizeof(vertices));
    if (quads <= 0) return;
    float top = vertices[0].y, bottom = top;
    for (int i = 1; i < quads * 4; ++i) {
        if (vertices[i].y < top) top = vertices[i].y;
        if (vertices[i].y > bottom) bottom = vertices[i].y;
    }
    draw_centered(x + w * 0.5f, y + h * 0.5f - (top + bottom) * 1.15f * 0.5f, 1.15f, label);
}

static bool handle_sound_click(double x, double y)
{
    if (point_in_rect(x, y, 640, SOUND_CONTROL_Y, 32, 28)) {
        if (g_music_muted || g_music_volume == 0) {
            g_music_muted = false;
            if (g_music_volume == 0) g_music_volume = MIX_MAX_VOLUME * 3 / 5;
        } else g_music_muted = true;
    } else if (point_in_rect(x, y, 684, SOUND_CONTROL_Y, 22, 28)) {
        g_music_volume = g_music_volume > 16 ? g_music_volume - 16 : 0;
        g_music_muted = false;
    } else if (point_in_rect(x, y, 766, SOUND_CONTROL_Y, 22, 28)) {
        g_music_volume = g_music_volume < MIX_MAX_VOLUME - 16 ? g_music_volume + 16 : MIX_MAX_VOLUME;
        g_music_muted = false;
    } else if (point_in_rect(x, y, 712, SOUND_CONTROL_Y, 45, 28)) {
        g_music_volume = (int)((x - 712 + 1) * MIX_MAX_VOLUME / 45);
        if (g_music_volume > MIX_MAX_VOLUME) g_music_volume = MIX_MAX_VOLUME;
        g_music_muted = false;
    } else return false;
    apply_music_volume();
    if (g_audio_ready && g_audio_focused && !g_music_muted && g_music_volume > 0 && g_tick_sound)
        Mix_PlayChannel(0, g_tick_sound, 0);
    else if (g_audio_ready) Mix_HaltChannel(0);
    return true;
}

static void draw_sound_controls(void)
{
    bool muted = g_music_muted || g_music_volume == 0;
    draw_button(640, SOUND_CONTROL_Y, 32, 28, "", false);
    glPushMatrix();
    glTranslatef(muted ? -4.375f : -1.875f, -0.5f, 0);
    glColor3f(0.91f, 0.96f, 1.0f);
    draw_rect(647, SOUND_CONTROL_Y + 11, 5, 7);
    glBegin(GL_QUADS);
    glVertex2f(652,SOUND_CONTROL_Y+11); glVertex2f(658,SOUND_CONTROL_Y+7);
    glVertex2f(658,SOUND_CONTROL_Y+22); glVertex2f(652,SOUND_CONTROL_Y+18);
    glEnd();
    glLineWidth(1.5f);
    if (muted) {
        glBegin(GL_LINES);
        glVertex2f(665,SOUND_CONTROL_Y+10); glVertex2f(673,SOUND_CONTROL_Y+19);
        glVertex2f(673,SOUND_CONTROL_Y+10); glVertex2f(665,SOUND_CONTROL_Y+19);
        glEnd();
    } else {
        // Same curved stroke, translated five pixels: both horizontal gaps are five pixels at the center.
        for (int copy = 0; copy < 2; ++copy) {
            glBegin(GL_QUAD_STRIP);
            for (int step = 0; step <= 24; ++step) {
                float angle = (-60.0f + step * 5.0f) * 0.01745329252f;
                for (int edge = 0; edge < 2; ++edge) {
                    float radius = edge ? 6.625f : 5.375f;
                    glVertex2f(657 + copy * 5 + cosf(angle) * radius,
                                SOUND_CONTROL_Y + 14.5f + sinf(angle) * radius);
                }
            }
            glEnd();
        }
    }
    glLineWidth(1);
    glPopMatrix();
    draw_button(684, SOUND_CONTROL_Y, 22, 28, "-", false);
    for (int segment = 0; segment < 5; ++segment) {
        bool on = !muted && g_music_volume > segment * MIX_MAX_VOLUME / 5;
        glColor3f(on ? 0.30f : 0.12f, on ? 0.82f : 0.20f, on ? 0.82f : 0.30f);
        draw_rect(712.0f + segment * 9.0f, SOUND_CONTROL_Y + 10, 7, 9);
    }
    draw_button(766, SOUND_CONTROL_Y, 22, 28, "+", false);
}

static void draw_menu_background(void)
{
    glClearColor(0.025f, 0.04f, 0.075f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0.06f, 0.09f, 0.14f); glBegin(GL_LINES);
    for (int x = 40; x < ARENA_W; x += 40)
    { glVertex2f((float)x, 0); glVertex2f((float)x, (float)ARENA_H); }
    for (int y = 40; y < ARENA_H; y += 40)
    { glVertex2f(0, (float)y); glVertex2f((float)ARENA_W, (float)y); }
    glEnd();
}

/* Hallmark component: name field; native pixel-game theme, no CSS runtime. */
static const struct {
    float surface[3], border[3], hover[3], focus[3], text[3], muted[3], error[3];
} name_field_theme = {
    {0.035f, 0.060f, 0.095f}, {0.35f, 0.44f, 0.55f}, {0.50f, 0.66f, 0.72f},
    {0.43f, 0.88f, 0.92f}, {0.91f, 0.96f, 1.0f}, {0.57f, 0.68f, 0.79f},
    {1.0f, 0.48f, 0.52f}
};

static void draw_name_field(double mx, double my)
{
    const float x = 290, y = menu_origin_y() + 90, w = 220, h = 40;
    bool hovered = point_in_rect(mx, my, x, y, w, h);
    bool invalid = strncmp(g_ui_error, "Enter your name", 15) == 0;
    const float *border = invalid ? name_field_theme.error :
        g_name_focused ? name_field_theme.focus : hovered ? name_field_theme.hover : name_field_theme.border;
    glColor3fv(name_field_theme.text); draw_text(x, text_y_at_center("Player name", 1.15f, y - 14), 1.15f, "Player name");
    char count[12]; snprintf(count, sizeof(count), "%zu/16", strlen(g_display_name));
    glColor3fv(name_field_theme.muted);
    draw_text(x + w - stb_easy_font_width(count), text_y_at_center(count, 1.0f, y - 14), 1.0f, count);
    glColor3fv(border); draw_rect(x, y, w, h);
    glColor3fv(name_field_theme.surface); draw_rect(x + 2, y + 2, w - 4, h - 4);
    glColor3fv(g_display_name[0] ? name_field_theme.text : name_field_theme.muted);
    const char *value = g_display_name[0] ? g_display_name : "Your nickname";
    if (g_name_focused && g_name_selected && g_display_name[0]) {
        float width = stb_easy_font_width(g_display_name) * 1.25f;
        glColor3fv(name_field_theme.focus); draw_rect(400 - width * 0.5f - 2, y + 10, width + 4, 20);
        glColor3fv(name_field_theme.surface);
    }
    draw_centered(400, text_y_at_center(value, 1.25f, y + h * 0.5f), 1.25f, value);
    if (g_name_focused && !g_name_selected) {
        float half_width = stb_easy_font_width((char *)value) * 1.25f * 0.5f;
        float caret_x = g_display_name[0] ? 400 + half_width + 2 : 400 - half_width - 3;
        glColor3fv(name_field_theme.focus); draw_rect(caret_x, y + (h - 17) * 0.5f, 2, 17);
    }
    glColor3fv(invalid ? name_field_theme.error : name_field_theme.muted);
    const char *help = invalid ? "Enter a name to create or join a room." : "Shown to everyone in your room.";
    draw_centered(400, text_y_at_center(help, 0.95f, menu_origin_y() + 145), 0.95f, help);
}

static void draw_main_menu(double mx, double my)
{
    draw_menu_background();
    float origin = menu_origin_y();
    double now = glfwGetTime();
    for (int i = 0; i < MENU_CAT_COUNT; ++i)
        update_and_draw_menu_cat(&g_menu_cats[i], now);
    glColor3f(0.43f, 0.88f, 0.92f); draw_centered(ARENA_W * 0.5f, text_y_at_center("COIN RUSH", 3.2f, origin), 3.2f, "COIN RUSH");
    glColor3f(0.68f, 0.77f, 0.88f); draw_centered(ARENA_W * 0.5f, text_y_at_center("CATCH COINS. CHASE THE HIGH SCORE.", 1.25f, origin + 38), 1.25f, "CATCH COINS. CHASE THE HIGH SCORE.");
    draw_name_field(mx, my);
    draw_button(250, origin + 168, 300, 48, "PLAY - CREATE ROOM", point_in_rect(mx, my, 250, origin + 168, 300, 48));
    draw_button(250, origin + 228, 300, 48, "JOIN A ROOM", point_in_rect(mx, my, 250, origin + 228, 300, 48));
    draw_button(250, origin + 288, 300, 40, "EXIT GAME", point_in_rect(mx, my, 250, origin + 288, 300, 40));
    glColor3f(0.54f, 0.65f, 0.78f);
    draw_centered(ARENA_W * 0.5f, text_y_at_center("UP TO FIVE PLAYERS  |  LEFT / RIGHT TO MOVE", 1.0f, origin + 350), 1.0f, "UP TO FIVE PLAYERS  |  LEFT / RIGHT TO MOVE");
    if (g_ui_error[0] && strncmp(g_ui_error, "Enter your name", 15) != 0)
    { glColor3f(1.0f, 0.48f, 0.52f); draw_centered(ARENA_W * 0.5f, origin + 373, 1.05f, g_ui_error); }
}

static void draw_exit_confirm(double mx, double my)
{
    draw_menu_background();
    glColor3f(0.99f, 0.62f, 0.39f); draw_centered(ARENA_W * 0.5f, 158, 2.5f, "LEAVE COIN RUSH?");
    glColor3f(0.04f, 0.065f, 0.11f); draw_rect(190, 215, 420, 250);
    glColor3f(0.72f, 0.80f, 0.90f);
    draw_centered(ARENA_W * 0.5f, 255, 1.15f, g_exit_from_game ? "You will leave this room and exit." : "Are you sure you want to exit?");
    draw_button(264, 316, 272, 54, "EXIT NOW",
                point_in_rect(mx, my, 264, 316, 272, 54));
    draw_button(264, 385, 272, 50, "CANCEL",
                point_in_rect(mx, my, 264, 385, 272, 50));
}

static void draw_join_code_screen(double mx, double my)
{
    draw_menu_background();
    const float top = (ARENA_H - 292) * 0.5f;
    glColor3f(0.04f, 0.065f, 0.11f); draw_rect(190, top, 420, 292);
    glColor3f(0.43f, 0.88f, 0.92f); draw_centered(400, text_y_at_center("JOIN A ROOM", 2.6f, top + 28), 2.6f, "JOIN A ROOM");
    glColor3f(0.68f, 0.77f, 0.88f); draw_centered(400, text_y_at_center("ENTER THE SIX-DIGIT ROOM CODE", 1.3f, top + 65), 1.3f, "ENTER THE SIX-DIGIT ROOM CODE");
    glColor3f(0.02f, 0.035f, 0.065f); draw_rect(264, top + 90, 272, 52);
    char display[7] = "______";
    for (size_t i = 0; i < g_room_code_length; ++i) display[i] = g_room_code_input[i];
    if (g_code_selected) {
        glColor3f(0.25f, 0.40f, 0.50f); draw_rect(350, top + 103, 100, 25);
    }
    glColor3f(1.0f, 0.83f, 0.42f); draw_centered(400, text_y_at_center(display, 2.0f, top + 116), 2.0f, display);
    draw_button(264, top + 162, 272, 48, "JOIN ROOM", point_in_rect(mx, my, 264, top + 162, 272, 48));
    draw_button(264, top + 224, 272, 44, "BACK", point_in_rect(mx, my, 264, top + 224, 272, 44));
    if (g_ui_error[0]) {
        glColor3f(1.0f, 0.48f, 0.52f); draw_centered(400, top + 309, 1.0f, g_ui_error);
    }
}

static void draw_connecting(uint32_t room_code, bool create_room)
{
    draw_menu_background();
    glColor3f(0.43f, 0.88f, 0.92f);
    bool quick_play = room_code == NET_QUICKPLAY_ROOM_CODE;
    draw_centered(ARENA_W * 0.5f, 172, 2.8f,
                  quick_play ? "CONNECTING TO GAME" :
                  (create_room ? "CREATING ROOM" : "JOINING ROOM"));
    glColor3f(0.04f, 0.065f, 0.11f); draw_rect(186, 235, 428, 184);
    char code[48]; snprintf(code, sizeof(code), "ROOM CODE  %06u", room_code);
    glColor3f(1.0f, 0.83f, 0.42f); draw_centered(ARENA_W * 0.5f, 279, 1.8f, code);
    glColor3f(0.70f, 0.79f, 0.89f); draw_centered(ARENA_W * 0.5f, 336, 1.15f, "Finding the room server...");
    draw_centered(ARENA_W * 0.5f, 368, 1.0f,
                  quick_play ? "Joining the server quick-play room..." :
                               "Share this code so friends can join your game.");
}

static void start_room_request(uint32_t room_code, bool create_room)
{
    bool has_letter = false;
    for (size_t i = 0; g_display_name[i]; ++i) has_letter |= g_display_name[i] != ' ';
    if (!has_letter) {
        snprintf(g_ui_error, sizeof(g_ui_error), "Enter your name before creating or joining a room.");
        g_screen = UI_MAIN_MENU; g_name_focused = true; return;
    }
    memset(g_pickup_effects, 0, sizeof(g_pickup_effects));
    pthread_mutex_lock(&g_mutex);
    snprintf(g_client.display_name, sizeof(g_client.display_name), "%s", g_display_name);
    g_client.room_code = room_code;
    g_client.create_room = create_room;
    g_client.client_token = dds_time();
    if (!g_client.client_token) g_client.client_token = 1;
    g_client.join_started = dds_time();
    g_client.join_failed = false;
    g_client.join_requested = true;
    g_client.player_id = 0;
    g_client.has_state = false;
    pthread_mutex_unlock(&g_mutex);
    g_ui_error[0] = '\0';
    g_screen = UI_CONNECTING;
}

static void submit_room_code(void)
{
    if (g_room_code_length != 6)
    { snprintf(g_ui_error, sizeof(g_ui_error), "Please enter all six digits."); return; }
    start_room_request((uint32_t)strtoul(g_room_code_input, NULL, 10), false);
}

static void character_callback(GLFWwindow *window, unsigned int codepoint)
{
    (void)window;
    if (g_screen == UI_MAIN_MENU && g_name_focused) {
        if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
            (codepoint >= '0' && codepoint <= '9') || codepoint == ' ' || codepoint == '_' || codepoint == '-') {
            if (g_name_selected) { g_display_name[0] = 0; g_name_selected = false; }
            size_t n = strlen(g_display_name);
            if (n < 16) { g_display_name[n] = (char)codepoint; g_display_name[n + 1] = 0; g_ui_error[0] = 0; }
        }
        return;
    }
    if (g_screen != UI_JOIN_CODE || codepoint < '0' || codepoint > '9') return;
    if (g_code_selected) { g_room_code_length = 0; g_code_selected = false; g_code_selected = false; }
    if (g_room_code_length >= 6) return;
    g_room_code_input[g_room_code_length++] = (char)codepoint;
    g_room_code_input[g_room_code_length] = 0; g_ui_error[0] = 0;
}

static void mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
    (void)mods;
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    double x, y; glfwGetCursorPos(window, &x, &y);
    window_to_scene(x, y, &g_click_x, &g_click_y);
    g_mouse_clicked = true;
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void)window; (void)scancode;
    if (action != GLFW_PRESS && !(action == GLFW_REPEAT &&
        (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE))) return;
    if (key == GLFW_KEY_F11)
    { toggle_fullscreen(window); return; }
    if (key == GLFW_KEY_ESCAPE && g_screen == UI_GAME)
    { g_exit_from_game = true; g_screen = UI_EXIT_CONFIRM; return; }
    if (g_screen == UI_GAME && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER))
    {
        pthread_mutex_lock(&g_mutex);
        if (g_client.has_state && (g_client.state.phase == 0 || g_client.state.phase == 2 || g_client.state.phase == 3))
            g_client.continue_requested = true;
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    if (g_screen == UI_EXIT_CONFIRM)
    {
        if (key == GLFW_KEY_ESCAPE)
            g_screen = g_exit_from_game ? UI_GAME : UI_MAIN_MENU;
        else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }
    if (g_screen == UI_MAIN_MENU && g_name_focused) {
        size_t n = strlen(g_display_name);
        if ((mods & GLFW_MOD_CONTROL) && key == GLFW_KEY_A) { g_name_selected = n > 0; return; }
        if ((key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE) && g_name_selected) {
            g_display_name[0] = 0; g_name_selected = false; g_ui_error[0] = 0;
        } else if (key == GLFW_KEY_BACKSPACE && n) { g_display_name[n - 1] = 0; g_ui_error[0] = 0; }
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) g_name_focused = false;
        return;
    }
    if (g_screen != UI_JOIN_CODE) return;
    if ((mods & GLFW_MOD_CONTROL) && key == GLFW_KEY_A) { g_code_selected = g_room_code_length > 0; return; }
    if ((key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE) && g_code_selected) {
        g_room_code_input[0] = 0; g_room_code_length = 0; g_code_selected = false; g_code_selected = false; return;
    }
    if (key == GLFW_KEY_BACKSPACE && g_room_code_length)
    { g_room_code_input[--g_room_code_length] = '\0'; g_ui_error[0] = '\0'; }
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) submit_room_code();
    else if (key == GLFW_KEY_ESCAPE)
    { g_room_code_length = 0; g_code_selected = false; g_room_code_input[0] = '\0'; g_ui_error[0] = '\0'; g_screen = UI_MAIN_MENU; }
}

static void *dds_worker(void *credentials)
{
    Network net;
    if (!net_open(&net, false, credentials))
    {
        pthread_mutex_lock(&g_mutex);
        g_client.network_failed = true;
        pthread_mutex_unlock(&g_mutex);
        return NULL;
    }

    uint64_t active_token = 0;
    while (1)
    {
        // Copy UI intent before calling DDS; discovery and reliable writes must
        // not hold the mutex needed by rendering or keyboard callbacks
        pthread_mutex_lock(&g_mutex);
        bool running = g_client.running, join_requested = g_client.join_requested;
        bool continue_requested = g_client.continue_requested;
        bool create_room = g_client.create_room;
        char direction = g_client.direction;
        uint32_t room_code = g_client.room_code;
        uint64_t token = g_client.client_token;
        char display_name[17];
        memcpy(display_name, g_client.display_name, sizeof(display_name));
        pthread_mutex_unlock(&g_mutex);
        if (!running) break;

        if (join_requested && token != active_token)
        {
            memcpy(net.display_name, display_name, sizeof(net.display_name));
            net_client_room(&net, (int32_t)room_code, create_room, token);
            active_token = token;
        }
        if (!join_requested)
        {
            // Cancelled joins must stop retrying; an abandoned admission expires
            // on the server even if its reply arrived after the UI timeout
            dds_sleepfor(DDS_MSECS(5));
            continue;
        }

        GameState incoming;
        int previous_player_id = net.player_id;
        bool updated = net_client_state(&net, &incoming);
        if (previous_player_id <= 0 && net.player_id > 0)
            printf("Joined room %06u as %s\n", room_code, display_name[0] ? display_name : "Unnamed player");
        if (!net_client_send(&net, continue_requested ? 'c' : direction))
        {
            pthread_mutex_lock(&g_mutex);
            g_client.network_failed = true;
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        pthread_mutex_lock(&g_mutex);
        g_client.player_id = net.player_id > 0 ? net.player_id : 0;
        if (net.player_id == 0)
        {
            g_client.join_failed = true;
            g_client.join_requested = false;
        }
        if (updated)
        {
            if ((incoming.phase != 0 && incoming.phase != 2 && incoming.phase != 3) ||
                (net.player_id > 0 && (incoming.ready_players & (1 << (net.player_id - 1)))))
                g_client.continue_requested = false;
            if (g_client.has_state)
                for (int p = 0; p < MAX_PLAYERS; ++p)
                {
                    if (incoming.player_x[p] < g_client.state.player_x[p]) g_player_facing[p] = -1;
                    else if (incoming.player_x[p] > g_client.state.player_x[p]) g_player_facing[p] = 1;
                }
            g_client.state = incoming; g_client.has_state = true;
        }
        else if (net.player_id <= 0)
            g_client.has_state = false;
        pthread_mutex_unlock(&g_mutex);
        dds_sleepfor(DDS_MSECS(5));
    }

    // The shared close sends a token-bound leave and waits briefly for its ACK
    net_close(&net);
    return NULL;
}

static int default_identity_lock = -1;

static bool acquire_identity_lock(int identity)
{
#if defined(__linux__)
    char lock_path[128];
    const unsigned long uid = (unsigned long)getuid();
    snprintf(lock_path, sizeof(lock_path), "/tmp/coin-rush-%lu-client%d.lock", uid, identity);
    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(fd);
        return false;
    }
    default_identity_lock = fd;
#else
    (void)identity;
#endif
    return true;
}

/* A local advisory lock gives bare `./client` launches a distinct signed
 * identity in this WSL install. An imported invite overrides the local default
 * so clients on other computers can use their host-issued certificate. */
static const char *select_default_credentials(void)
{
#if defined(__linux__)
    static char selected[4096];
    char credential_root[4096] = "";
    char preference_path[4096] = "";
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (data_home && data_home[0])
    {
        snprintf(credential_root, sizeof(credential_root),
                 "%s/coinrush/credentials-invites", data_home);
        snprintf(preference_path, sizeof(preference_path), "%s/default-player",
                 credential_root);
    }
    else if (home && home[0])
    {
        snprintf(credential_root, sizeof(credential_root),
                 "%s/.local/share/coinrush/credentials-invites", home);
        snprintf(preference_path, sizeof(preference_path), "%s/default-player",
                 credential_root);
    }

    FILE *preference = preference_path[0] ? fopen(preference_path, "r") : NULL;
    if (!preference)
    {
        /* Compatibility with invites imported by older project versions. */
        snprintf(credential_root, sizeof(credential_root), "credentials-invites");
        snprintf(preference_path, sizeof(preference_path),
                 "credentials-invites/default-player");
        preference = fopen(preference_path, "r");
    }
    if (preference)
    {
        int identity = 0;
        bool valid = fscanf(preference, "%d", &identity) == 1 && identity >= 2 && identity <= 6;
        fclose(preference);
        if (!valid)
        {
            fprintf(stderr, "Invalid imported client identity setting.\n");
            return NULL;
        }
        snprintf(selected, sizeof(selected), "%s/client%d", credential_root, identity);
        char certificate[sizeof(selected) + 16];
        snprintf(certificate, sizeof(certificate), "%s/identity.pem", selected);
        if (access(certificate, R_OK) != 0)
        {
            fprintf(stderr, "Imported player identity client%d is missing; re-import its invite.\n",
                    identity);
            return NULL;
        }
        if (!acquire_identity_lock(identity))
        {
            fprintf(stderr, "Imported player identity client%d is already in use.\n", identity);
            return NULL;
        }
        fprintf(stderr, "Using imported player identity client%d\n", identity);
        return selected;
    }

    for (int identity = 1; identity <= MAX_PLAYERS; ++identity) {
        snprintf(selected, sizeof(selected), "credentials-private/client%d", identity);
        char certificate[sizeof(selected) + 16];
        snprintf(certificate, sizeof(certificate), "%s/identity.pem", selected);
        if (access(certificate, R_OK) != 0)
            continue;

        if (acquire_identity_lock(identity)) {
            fprintf(stderr, "Using local player identity client%d\n", identity);
            return selected;
        }
    }

    fprintf(stderr, "No free local player identity is available (maximum %d clients).\n",
            MAX_PLAYERS);
    return NULL;
#else
    return "credentials-private/client1";
#endif
}

int main(int argc, char **argv)
{
    const char *credentials = argc > 1 ? argv[1] : select_default_credentials();
    if (!credentials)
        return 1;
    bool software_renderer = configure_wslg_renderer();
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) { fprintf(stderr, "GLFW could not initialize.\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(ARENA_W, ARENA_H, "Coin Rush", NULL, NULL);
    if (!window) { fprintf(stderr, "Could not open Coin Rush.\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    uint64_t seed = dds_time() ^ ((uint64_t)(uintptr_t)window * UINT64_C(0x9e3779b97f4a7c15));
    srand((unsigned int)(seed ^ (seed >> 32)));
    glfwSwapInterval(software_renderer ? 2 : 1);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
    update_viewport(window, fbw, fbh);
    glDrawBuffer(GL_BACK); glReadBuffer(GL_BACK);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    GLuint cat_east = load_cat_texture("east"), cat_west = load_cat_texture("west");
    g_tofu_texture = load_rgba_texture(TOFU_ASSET_PATH, TOFU_SHEET_WIDTH, TOFU_SHEET_HEIGHT);
    g_cato_walk_texture = load_rgba_texture(CATO_WALK_ASSET_PATH, CATO_SHEET_WIDTH, CATO_SHEET_HEIGHT);
    g_cato_jump_texture = load_rgba_texture(CATO_JUMP_ASSET_PATH, CATO_SHEET_WIDTH, CATO_SHEET_HEIGHT);
    g_bomb_texture = load_rgba_texture(BOMB_SHEET_PATH, BOMB_SHEET_WIDTH, BOMB_SHEET_HEIGHT);
    g_plus_texture = load_rgba_texture(PLUS_SHEET_PATH, PLUS_SHEET_WIDTH, PLUS_SHEET_HEIGHT);
    g_heart_texture = load_rgba_texture(HEART_SHEET_PATH, HEART_SHEET_WIDTH, HEART_SHEET_HEIGHT);
    initialize_menu_cats(glfwGetTime());
    initialize_audio();
    glfwSetWindowFocusCallback(window, audio_focus_callback);
    audio_focus_callback(window, glfwGetWindowAttrib(window, GLFW_FOCUSED));

    pthread_mutex_lock(&g_mutex); g_client.running = true; g_client.direction = 'n'; pthread_mutex_unlock(&g_mutex);
    pthread_t worker;
    if (pthread_create(&worker, NULL, dds_worker, (void *)credentials) != 0)
    { fprintf(stderr, "Could not start the DDS worker.\n"); glfwDestroyWindow(window); glfwTerminate(); return 1; }
    glfwSetCharCallback(window, character_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        double window_mx = 0, window_my = 0, mx = 0, my = 0;
        glfwGetCursorPos(window, &window_mx, &window_my);
        window_to_scene(window_mx, window_my, &mx, &my);
        bool clicked = g_mouse_clicked; g_mouse_clicked = false;
        if (clicked) { mx = g_click_x; my = g_click_y; }

        if (g_screen == UI_MAIN_MENU && clicked)
        {
            g_name_selected = false;
            g_name_focused = point_in_rect(mx, my, 290, menu_origin_y() + 90, 220, 40);
            if (point_in_rect(mx, my, 250, menu_origin_y() + 168, 300, 48))
            {
                uint32_t room_code = (uint32_t)NET_QUICKPLAY_ROOM_CODE + 1u +
                                     (uint32_t)(rand() % 899999);
                start_room_request(room_code, true);
            }
            else if (point_in_rect(mx, my, 290, menu_origin_y() + 90, 220, 40)) g_name_focused = true;
            else if (point_in_rect(mx, my, 250, menu_origin_y() + 228, 300, 48))
            { g_screen = UI_JOIN_CODE; g_room_code_length = 0; g_code_selected = false; g_room_code_input[0] = '\0'; g_ui_error[0] = '\0'; }
            else if (point_in_rect(mx, my, 250, menu_origin_y() + 288, 300, 40))
            { g_exit_from_game = false; g_screen = UI_EXIT_CONFIRM; }
        }
        else if (g_screen == UI_JOIN_CODE && clicked)
        {
            if (point_in_rect(mx, my, 264, (ARENA_H - 292) * 0.5f + 162, 272, 48)) submit_room_code();
            else if (point_in_rect(mx, my, 264, (ARENA_H - 292) * 0.5f + 224, 272, 44))
            { g_screen = UI_MAIN_MENU; g_room_code_length = 0; g_code_selected = false; g_room_code_input[0] = '\0'; g_ui_error[0] = '\0'; }
        }
        else if (g_screen == UI_EXIT_CONFIRM && clicked)
        {
            if (point_in_rect(mx, my, 264, 316, 272, 54))
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            else if (point_in_rect(mx, my, 264, 385, 272, 50))
                g_screen = g_exit_from_game ? UI_GAME : UI_MAIN_MENU;
        }
        else if (g_screen == UI_GAME && clicked)
        {
            if (handle_sound_click(mx, my)) { /* handled by audio controls */ }
            else
            {
                pthread_mutex_lock(&g_mutex);
                if (g_client.has_state &&
                    (g_client.state.phase == 0 || g_client.state.phase == 2 || g_client.state.phase == 3) &&
                    point_in_rect(mx, my, 264, ready_button_y(&g_client.state), 272, 48))
                    g_client.continue_requested = true;
                pthread_mutex_unlock(&g_mutex);
            }
        }

        pthread_mutex_lock(&g_mutex);
        int player_id = g_client.player_id;
        bool network_failed = g_client.network_failed;
        bool join_failed = g_client.join_failed;
        bool create_room = g_client.create_room;
        uint32_t room_code = g_client.room_code;
        uint64_t join_started = g_client.join_started;
        GameState state = g_client.state;
        bool has_state = g_client.has_state;
        int facing[MAX_PLAYERS]; memcpy(facing, g_player_facing, sizeof(facing));
        g_client.direction = 'n';
        if (g_screen == UI_GAME && (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS))
            g_client.direction = 'l';
        else if (g_screen == UI_GAME && (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS))
            g_client.direction = 'r';
        pthread_mutex_unlock(&g_mutex);

        double frame_time = glfwGetTime();
        update_game_music(&state, has_state);
        if (has_state)
            observe_coin_collects(&state, frame_time);

        if (network_failed)
        {
            fprintf(stderr, "Secure DDS connection failed; check this client's credentials\n");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        if (g_screen == UI_CONNECTING && player_id > 0) g_screen = UI_GAME;
        if (g_screen == UI_CONNECTING && player_id == 0 &&
            dds_time() - join_started > DDS_SECS(12))
        {
            pthread_mutex_lock(&g_mutex);
            g_client.join_failed = true;
            g_client.join_requested = false;
            pthread_mutex_unlock(&g_mutex);
            join_failed = true;
        }
        if (g_screen == UI_CONNECTING && join_failed)
        {
            pthread_mutex_lock(&g_mutex); g_client.join_failed = false; pthread_mutex_unlock(&g_mutex);
            snprintf(g_ui_error, sizeof(g_ui_error), "No server reply. Check both PCs' WSL network and firewall.");
            g_screen = create_room ? UI_MAIN_MENU : UI_JOIN_CODE;
        }

        glLoadIdentity();
        if (g_screen == UI_MAIN_MENU) draw_main_menu(mx, my);
        else if (g_screen == UI_JOIN_CODE) draw_join_code_screen(mx, my);
        else if (g_screen == UI_CONNECTING) draw_connecting(room_code, create_room);
        else if (g_screen == UI_EXIT_CONFIRM) draw_exit_confirm(mx, my);
        else draw_arena(&state, has_state, player_id, room_code, facing,
                        cat_east, cat_west, frame_time);
        glfwSwapBuffers(window);
    }

    pthread_mutex_lock(&g_mutex); g_client.running = false; pthread_mutex_unlock(&g_mutex);
    pthread_join(worker, NULL);
    if (g_audio_ready)
    {
        Mix_HaltMusic();
        Mix_HaltChannel(-1);
        if (g_coin_sound) Mix_FreeChunk(g_coin_sound);
        if (g_bomb_sound) Mix_FreeChunk(g_bomb_sound);
        if (g_tick_sound) Mix_FreeChunk(g_tick_sound);
        if (g_level_one_music) Mix_FreeMusic(g_level_one_music);
        if (g_level_two_music) Mix_FreeMusic(g_level_two_music);
        if (g_level_three_music) Mix_FreeMusic(g_level_three_music);
        if (g_bonus_music) Mix_FreeMusic(g_bonus_music);
        Mix_CloseAudio();
        Mix_Quit();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (cat_east) glDeleteTextures(1, &cat_east);
    if (cat_west) glDeleteTextures(1, &cat_west);
    if (g_tofu_texture) glDeleteTextures(1, &g_tofu_texture);
    if (g_cato_walk_texture) glDeleteTextures(1, &g_cato_walk_texture);
    if (g_cato_jump_texture) glDeleteTextures(1, &g_cato_jump_texture);
    glfwDestroyWindow(window); glfwTerminate();
    return g_client.network_failed ? 1 : 0;
}
