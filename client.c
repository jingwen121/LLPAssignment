/* Coin Rush: GLFW/OpenGL client with DDS room-code matchmaking. */
#if defined(__linux__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <GLFW/glfw3.h>
#include <dds/dds.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "messages.h"
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

typedef enum { UI_MAIN_MENU, UI_JOIN_CODE, UI_CONNECTING, UI_GAME, UI_EXIT_CONFIRM } UiScreen;

typedef struct
{
    int player_id;
    char direction;
    bool running;
    bool join_requested;
    bool create_room;
    bool join_failed;
    uint32_t room_code;
    uint64_t client_token;
    uint64_t join_started;
    GameState state;
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
static MenuCat g_menu_cats[MENU_CAT_COUNT];
static GLuint g_tofu_texture;
static GLuint g_cato_walk_texture;
static GLuint g_cato_jump_texture;

static void glfw_error_callback(int code, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, description ? description : "unknown error");
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
    draw_text(centre - stb_easy_font_width((char *)text) * scale * 0.5f, y, scale, text);
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

static void draw_player(const GameState *state, int i, int my_id, int facing,
                        GLuint cat_east, GLuint cat_west)
{
    static const float tints[MAX_PLAYERS][3] = {
        {0.62f, 0.88f, 1.00f}, {1.00f, 0.58f, 0.72f}, {0.78f, 0.65f, 1.00f},
        {1.00f, 0.84f, 0.48f}, {0.55f, 1.00f, 0.67f}};
    if (!state->player_active[i])
        return;
    int x = state->player_x[i], y = state->player_y[i];
    const float *tint = tints[i];
    GLuint sprite = facing < 0 ? cat_west : cat_east;
    if (sprite)
        draw_cat(sprite, (float)x - 7.0f, (float)y - 28.0f, tint);
    else
    {
        glColor3f(tint[0], tint[1], tint[2]);
        draw_rect((float)x, (float)y, PLAYER_W, PLAYER_H);
        char label[8];
        snprintf(label, sizeof(label), "P%d", i + 1);
        glColor3f(0.04f, 0.07f, 0.12f);
        draw_centered((float)x + 25.0f, (float)y + 20.0f, 1.5f, label);
    }
    if (my_id == i + 1)
    {
        glColor3f(0.68f, 0.91f, 1.0f);
        draw_centered((float)x + 25.0f, (float)y - 42.0f, 1.1f, "YOU");
    }
}

static const char *phase_name(int phase)
{
    if (phase == 1) return "COIN RUSH";
    if (phase == 2) return "ROUND COMPLETE";
    return "ROOM LOBBY";
}

static void draw_arena(const GameState *state, bool has_state, int player_id,
                       uint32_t room_code, const int facing[MAX_PLAYERS],
                       GLuint cat_east, GLuint cat_west)
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
    glColor3f(0.88f, 0.93f, 1.0f); draw_centered(330, 20, 1.6f, phase_name(state->phase));
    glColor3f(1.0f, 0.83f, 0.42f);
    snprintf(text, sizeof(text), "TIME  %02d", state->time_left); draw_centered(525, 20, 1.8f, text);
    glColor3f(0.73f, 0.81f, 0.91f);
    snprintf(text, sizeof(text), "PLAYERS  %d/%d", active_count, MAX_PLAYERS);
    draw_centered(710, 22, 1.2f, text);

    if (state->phase == 1 && g_viewport.height > 0)
    {
        int playfield_height = (int)lroundf((float)g_viewport.height *
                                            (float)(ARENA_H - HUD_H) / (float)ARENA_H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(g_viewport.x, g_viewport.y, g_viewport.width, playfield_height);
        for (int i = 0; i < MAX_COINS; ++i)
            if (state->coin_active[i])
                draw_coin(state->coin_x[i], state->coin_y[i], state->coin_size[i], state->coin_gold[i]);
        glDisable(GL_SCISSOR_TEST);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
        draw_player(state, i, player_id, facing[i], cat_east, cat_west);

    if (state->phase == 0)
    {
        glColor3f(0.05f, 0.08f, 0.13f); draw_rect(170, 122, 460, 188);
        glColor3f(0.16f, 0.24f, 0.34f); draw_rect(170, 122, 460, 3);
        glColor3f(0.92f, 0.96f, 1); draw_centered(ARENA_W * 0.5f, 151, 2.1f, "ROOM LOBBY");
        glColor3f(0.67f, 0.77f, 0.89f);
        snprintf(text, sizeof(text), "%d / 2 PLAYERS - WAITING TO START", active_count);
        draw_centered(ARENA_W * 0.5f, 194, 1.25f, text);
        glColor3f(1, 0.83f, 0.42f);
        snprintf(text, sizeof(text), "ROOM CODE  %06u", room_code); draw_centered(ARENA_W * 0.5f, 233, 1.65f, text);
        glColor3f(0.57f, 0.68f, 0.81f);
        draw_centered(ARENA_W * 0.5f, 274, 1.05f, "Share this code with friends to fill the room");
    }
    else if (state->phase == 2)
    {
        glColor3f(0.04f, 0.07f, 0.12f); draw_rect(150, 160, 500, 232);
        glColor3f(0.99f, 0.73f, 0.27f); draw_rect(150, 160, 500, 3);
        glColor3f(0.99f, 0.83f, 0.39f); draw_centered(ARENA_W * 0.5f, 193, 2.2f, "ROUND COMPLETE");
        char winners[96] = ""; size_t used = 0;
        for (int i = 0; i < MAX_PLAYERS; ++i)
            if ((state->winners & (1 << i)) != 0)
            {
                int n = snprintf(winners + used, sizeof(winners) - used,
                                 "%sP%d", used ? " & " : "", i + 1);
                if (n > 0 && (size_t)n < sizeof(winners) - used) used += (size_t)n;
            }
        if (!used) snprintf(winners, sizeof(winners), "No winner");
        glColor3f(0.91f, 0.95f, 1); draw_centered(ARENA_W * 0.5f, 225, 1.4f, "WINNER");
        draw_centered(ARENA_W * 0.5f, 252, 1.8f, winners);
        glColor3f(0.67f, 0.77f, 0.89f);
        snprintf(text, sizeof(text), "TOP SCORE  %d PTS", state->best_score);
        draw_centered(ARENA_W * 0.5f, 283, 1.4f, text);
        glColor3f(0.45f, 0.58f, 0.73f);
        snprintf(text, sizeof(text), "NEXT ROUND IN %d", state->time_left);
        draw_centered(ARENA_W * 0.5f, 335, 1.15f, text);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (state->player_active[i])
        {
            snprintf(text, sizeof(text), "P%d  %d pts%s", i + 1, state->player_score[i],
                     player_id == i + 1 ? "  YOU" : "");
            glColor3f(player_id == i + 1 ? 0.61f : 0.75f,
                      player_id == i + 1 ? 0.89f : 0.82f, 0.97f);
            draw_centered(((float)i + 0.5f) * ARENA_W / MAX_PLAYERS, (float)ARENA_H - 22.0f, 1.15f, text);
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
    float text_height = stb_easy_font_height((char *)label) * 1.15f;
    draw_centered(x + w * 0.5f, y + (h - text_height) * 0.5f, 1.15f, label);
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

static void draw_main_menu(double mx, double my)
{
    draw_menu_background();
    double now = glfwGetTime();
    for (int i = 0; i < MENU_CAT_COUNT; ++i)
        update_and_draw_menu_cat(&g_menu_cats[i], now);
    glColor3f(0.43f, 0.88f, 0.92f); draw_centered(ARENA_W * 0.5f, 105, 3.2f, "COIN RUSH");
    glColor3f(0.68f, 0.77f, 0.88f); draw_centered(ARENA_W * 0.5f, 158, 1.25f, "CATCH COINS. CHASE THE HIGH SCORE.");
    glColor3f(0.04f, 0.065f, 0.11f); draw_rect(220, 208, 360, 340);
    draw_button(278, 244, 244, 56, "PLAY - CREATE ROOM", point_in_rect(mx, my, 278, 244, 244, 56));
    draw_button(278, 318, 244, 56, "JOIN A ROOM", point_in_rect(mx, my, 278, 318, 244, 56));
    draw_button(278, 392, 244, 56, "EXIT GAME", point_in_rect(mx, my, 278, 392, 244, 56));
    glColor3f(0.54f, 0.65f, 0.78f);
    draw_centered(ARENA_W * 0.5f, 480, 1.0f, "UP TO FIVE PLAYERS  |  LEFT / RIGHT TO MOVE");
    if (g_ui_error[0])
    { glColor3f(1.0f, 0.48f, 0.52f); draw_centered(ARENA_W * 0.5f, 505, 1.05f, g_ui_error); }
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
    glColor3f(0.43f, 0.88f, 0.92f); draw_centered(ARENA_W * 0.5f, 105, 2.6f, "JOIN A ROOM");
    glColor3f(0.04f, 0.065f, 0.11f); draw_rect(190, 190, 420, 280);
    glColor3f(0.68f, 0.77f, 0.88f); draw_centered(ARENA_W * 0.5f, 232, 1.3f, "ENTER THE SIX-DIGIT ROOM CODE");
    glColor3f(0.02f, 0.035f, 0.065f); draw_rect(264, 276, 272, 58);
    char display[7] = "______";
    for (size_t i = 0; i < g_room_code_length; ++i) display[i] = g_room_code_input[i];
    glColor3f(1.0f, 0.83f, 0.42f); draw_centered(ARENA_W * 0.5f, 296, 2.0f, display);
    draw_button(264, 357, 272, 54, "JOIN ROOM", point_in_rect(mx, my, 264, 357, 272, 54));
    draw_button(264, 424, 272, 48, "BACK", point_in_rect(mx, my, 264, 424, 272, 48));
    if (g_ui_error[0])
    { glColor3f(1.0f, 0.48f, 0.52f); draw_centered(ARENA_W * 0.5f, 500, 1.0f, g_ui_error); }
}

static void draw_connecting(uint32_t room_code, bool create_room)
{
    draw_menu_background();
    glColor3f(0.43f, 0.88f, 0.92f);
    draw_centered(ARENA_W * 0.5f, 172, 2.8f, create_room ? "CREATING ROOM" : "JOINING ROOM");
    glColor3f(0.04f, 0.065f, 0.11f); draw_rect(186, 235, 428, 184);
    char code[48]; snprintf(code, sizeof(code), "ROOM CODE  %06u", room_code);
    glColor3f(1.0f, 0.83f, 0.42f); draw_centered(ARENA_W * 0.5f, 279, 1.8f, code);
    glColor3f(0.70f, 0.79f, 0.89f); draw_centered(ARENA_W * 0.5f, 336, 1.15f, "Finding the room server...");
    draw_centered(ARENA_W * 0.5f, 368, 1.0f, "Share this code so friends can join your game.");
}

static void start_room_request(uint32_t room_code, bool create_room)
{
    pthread_mutex_lock(&g_mutex);
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
    if (g_screen != UI_JOIN_CODE || codepoint < '0' || codepoint > '9' || g_room_code_length >= 6)
        return;
    g_room_code_input[g_room_code_length++] = (char)codepoint;
    g_room_code_input[g_room_code_length] = '\0';
    g_ui_error[0] = '\0';
}

static void key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void)window; (void)scancode; (void)mods;
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_F11)
    { toggle_fullscreen(window); return; }
    if (key == GLFW_KEY_ESCAPE && g_screen == UI_GAME)
    { g_exit_from_game = true; g_screen = UI_EXIT_CONFIRM; return; }
    if (g_screen == UI_EXIT_CONFIRM)
    {
        if (key == GLFW_KEY_ESCAPE)
            g_screen = g_exit_from_game ? UI_GAME : UI_MAIN_MENU;
        else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }
    if (g_screen != UI_JOIN_CODE) return;
    if (key == GLFW_KEY_BACKSPACE && g_room_code_length)
    { g_room_code_input[--g_room_code_length] = '\0'; g_ui_error[0] = '\0'; }
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) submit_room_code();
    else if (key == GLFW_KEY_ESCAPE)
    { g_room_code_length = 0; g_room_code_input[0] = '\0'; g_ui_error[0] = '\0'; g_screen = UI_MAIN_MENU; }
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
        bool create_room = g_client.create_room;
        char direction = g_client.direction;
        uint32_t room_code = g_client.room_code;
        uint64_t token = g_client.client_token;
        pthread_mutex_unlock(&g_mutex);
        if (!running) break;

        if (join_requested && token != active_token)
        {
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
            printf("Joined room %06u as player %d\n", room_code, net.player_id);
        if (!net_client_send(&net, direction))
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

int main(int argc, char **argv)
{
    const char *credentials = argc > 1 ? argv[1] : "credentials-private/client1";
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
    initialize_menu_cats(glfwGetTime());

    pthread_mutex_lock(&g_mutex); g_client.running = true; g_client.direction = 'n'; pthread_mutex_unlock(&g_mutex);
    pthread_t worker;
    if (pthread_create(&worker, NULL, dds_worker, (void *)credentials) != 0)
    { fprintf(stderr, "Could not start the DDS worker.\n"); glfwDestroyWindow(window); glfwTerminate(); return 1; }
    glfwSetCharCallback(window, character_callback);
    glfwSetKeyCallback(window, key_callback);

    bool previous_mouse_down = false;
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        double window_mx = 0, window_my = 0, mx = 0, my = 0;
        glfwGetCursorPos(window, &window_mx, &window_my);
        window_to_scene(window_mx, window_my, &mx, &my);
        bool mouse_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool clicked = mouse_down && !previous_mouse_down; previous_mouse_down = mouse_down;

        if (g_screen == UI_MAIN_MENU && clicked)
        {
            if (point_in_rect(mx, my, 278, 244, 244, 56))
            {
                uint32_t room_code = 100000u + (uint32_t)(rand() % 900000);
                start_room_request(room_code, true);
            }
            else if (point_in_rect(mx, my, 278, 318, 244, 56))
            { g_screen = UI_JOIN_CODE; g_room_code_length = 0; g_room_code_input[0] = '\0'; g_ui_error[0] = '\0'; }
            else if (point_in_rect(mx, my, 278, 392, 244, 56))
            { g_exit_from_game = false; g_screen = UI_EXIT_CONFIRM; }
        }
        else if (g_screen == UI_JOIN_CODE && clicked)
        {
            if (point_in_rect(mx, my, 264, 357, 272, 54)) submit_room_code();
            else if (point_in_rect(mx, my, 264, 424, 272, 48))
            { g_screen = UI_MAIN_MENU; g_room_code_length = 0; g_room_code_input[0] = '\0'; g_ui_error[0] = '\0'; }
        }
        else if (g_screen == UI_EXIT_CONFIRM && clicked)
        {
            if (point_in_rect(mx, my, 264, 316, 272, 54))
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            else if (point_in_rect(mx, my, 264, 385, 272, 50))
                g_screen = g_exit_from_game ? UI_GAME : UI_MAIN_MENU;
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
            snprintf(g_ui_error, sizeof(g_ui_error), "Room unavailable, full, or server not running.");
            g_screen = create_room ? UI_MAIN_MENU : UI_JOIN_CODE;
        }

        glLoadIdentity();
        if (g_screen == UI_MAIN_MENU) draw_main_menu(mx, my);
        else if (g_screen == UI_JOIN_CODE) draw_join_code_screen(mx, my);
        else if (g_screen == UI_CONNECTING) draw_connecting(room_code, create_room);
        else if (g_screen == UI_EXIT_CONFIRM) draw_exit_confirm(mx, my);
        else draw_arena(&state, has_state, player_id, room_code, facing, cat_east, cat_west);
        glfwSwapBuffers(window);
    }

    pthread_mutex_lock(&g_mutex); g_client.running = false; pthread_mutex_unlock(&g_mutex);
    pthread_join(worker, NULL);
    if (cat_east) glDeleteTextures(1, &cat_east);
    if (cat_west) glDeleteTextures(1, &cat_west);
    if (g_tofu_texture) glDeleteTextures(1, &g_tofu_texture);
    if (g_cato_walk_texture) glDeleteTextures(1, &g_cato_walk_texture);
    if (g_cato_jump_texture) glDeleteTextures(1, &g_cato_jump_texture);
    glfwDestroyWindow(window); glfwTerminate();
    return g_client.network_failed ? 1 : 0;
}
