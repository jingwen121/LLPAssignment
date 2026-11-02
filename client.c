/* Coin Rush client: GLFW input, DDS state subscription and OpenGL rendering.
 * Build from WSL in the project directory with the command in README.md. */

#if defined(__linux__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <GLFW/glfw3.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "dds/dds.h"
#include "messages.h"
#include "stb_easy_font.h"

#define STB_EASY_FONT_IMPLEMENTATION

#define MAX_SAMPLES 1
#define ARENA_W 800
#define ARENA_H 600
#define HUD_H 76
#define PLAYER_W 50
#define PLAYER_H 50
#define MAX_PLAYERS 5
#define MAX_COINS 8

typedef struct
{
    int player_id;
    char direction;
    bool running;
    GameState state;
    bool has_state;
} ClientData;

static ClientData g_client;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void glfw_error_callback(int code, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, description ? description : "unknown error");
}

static void configure_wslg_renderer(void)
{
#if defined(__linux__)
    const char *wsl = getenv("WSL_DISTRO_NAME");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *software_gl = getenv("LIBGL_ALWAYS_SOFTWARE");
    if (wsl && *wsl && wayland && *wayland && (!software_gl || !*software_gl))
    {
        if (setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1) == 0)
            fprintf(stderr, "WSLg detected; using Mesa software OpenGL for this client.\n");
    }
#endif
}

static void draw_rect(float x, float y, float w, float h)
{
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void draw_text(float x, float y, float scale, const char *text)
{
    /* stb_easy_font emits one 16-byte vertex record per corner. Leave enough
     * room for the longest HUD string so the library never truncates labels. */
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

static void draw_coin(int x, int y, int size, bool gold)
{
    const int segments = 20;
    const float radius = size * 0.5f;
    const float cx = x + radius;
    const float cy = y + radius;

    if (gold)
        glColor3f(1.0f, 0.78f, 0.19f);
    else
        glColor3f(0.33f, 0.91f, 0.72f);

    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segments; ++i)
    {
        float angle = (float)i * 6.28318530718f / (float)segments;
        glVertex2f(cx + radius * cosf(angle), cy + radius * sinf(angle));
    }
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < segments; ++i)
    {
        float angle = (float)i * 6.28318530718f / (float)segments;
        glVertex2f(cx + radius * 0.68f * cosf(angle), cy + radius * 0.68f * sinf(angle));
    }
    glEnd();
}

static void draw_player(const GameState *state, int index, int my_id)
{
    static const float colors[MAX_PLAYERS][3] = {
        {0.31f, 0.73f, 1.00f}, {1.00f, 0.42f, 0.49f}, {0.64f, 0.50f, 1.00f},
        {1.00f, 0.63f, 0.27f}, {0.27f, 0.85f, 0.58f}};
    if (!state->player_active[index])
        return;

    int x = state->player_x[index];
    int y = state->player_y[index];
    const float *color = colors[index];

    glColor3f(0.02f, 0.04f, 0.08f);
    draw_rect((float)x + 3.0f, (float)y + 4.0f, PLAYER_W, PLAYER_H);
    glColor3f(color[0], color[1], color[2]);
    draw_rect((float)x, (float)y, PLAYER_W, PLAYER_H);
    glColor3f(0.09f, 0.13f, 0.20f);
    draw_rect((float)x + 5.0f, (float)y + 7.0f, PLAYER_W - 10.0f, 8.0f);

    glColor3f(0.04f, 0.07f, 0.12f);
    char label[24];
    snprintf(label, sizeof(label), "P%d", index + 1);
    draw_text((float)x + 10.0f, (float)y + 24.0f, 1.8f, label);
    if (my_id == index + 1)
    {
        glColor3f(0.68f, 0.91f, 1.0f);
        draw_text((float)x + 3.0f, (float)y - 14.0f, 1.1f, "YOU");
    }
}

static const char *phase_name(int phase)
{
    switch (phase)
    {
        case 1: return "COIN RUSH";
        case 2: return "ROUND COMPLETE";
        default: return "WAITING ROOM";
    }
}

static void draw_arena(const GameState *state, bool has_state, int player_id)
{
    glClearColor(0.035f, 0.055f, 0.095f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* HUD band and arena frame */
    glColor3f(0.07f, 0.10f, 0.16f);
    draw_rect(0.0f, 0.0f, ARENA_W, HUD_H);
    glColor3f(0.13f, 0.19f, 0.28f);
    draw_rect(0.0f, HUD_H - 2.0f, ARENA_W, 2.0f);
    glColor3f(0.10f, 0.14f, 0.21f);
    for (int x = 20; x < ARENA_W; x += 40)
        draw_rect((float)x, (float)HUD_H, 1.0f, ARENA_H - HUD_H);
    for (int y = HUD_H + 20; y < ARENA_H; y += 40)
        draw_rect(0.0f, (float)y, ARENA_W, 1.0f);

    glColor3f(0.43f, 0.88f, 0.92f);
    draw_text(24.0f, 20.0f, 2.1f, "COIN RUSH");

    char line[96];
    if (!has_state)
    {
        glColor3f(0.90f, 0.94f, 1.0f);
        draw_text(24.0f, 106.0f, 2.2f, "CONNECTING TO GAME SERVER...");
        glColor3f(0.62f, 0.70f, 0.80f);
        draw_text(24.0f, 145.0f, 1.5f, "Joining the DDS game. Please wait.");
        return;
    }

    int active_count = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        active_count += state->player_active[i] ? 1 : 0;

    snprintf(line, sizeof(line), "%s", phase_name(state->phase));
    glColor3f(0.88f, 0.93f, 1.0f);
    draw_text(250.0f, 20.0f, 1.6f, line);
    snprintf(line, sizeof(line), "TIME  %02d", state->time_left);
    glColor3f(1.0f, 0.83f, 0.42f);
    draw_text(476.0f, 20.0f, 1.8f, line);
    snprintf(line, sizeof(line), "PLAYERS  %d/%d", active_count, MAX_PLAYERS);
    glColor3f(0.73f, 0.81f, 0.91f);
    draw_text(650.0f, 22.0f, 1.2f, line);

    if (state->phase == 0)
    {
        glColor3f(0.92f, 0.96f, 1.0f);
        draw_text(24.0f, 108.0f, 2.2f, "WAITING FOR PLAYERS");
        glColor3f(0.67f, 0.77f, 0.89f);
        snprintf(line, sizeof(line), "%d of %d players needed to start", active_count, 2);
        draw_text(24.0f, 148.0f, 1.6f, line);
        glColor3f(0.45f, 0.58f, 0.73f);
        draw_text(24.0f, 178.0f, 1.35f, "Move with LEFT / RIGHT arrows or A / D");
    }
    else if (state->phase == 1)
    {
        for (int i = 0; i < MAX_COINS; ++i)
            if (state->coin_active[i])
                draw_coin(state->coin_x[i], state->coin_y[i], state->coin_size[i], state->coin_gold[i]);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i)
        draw_player(state, i, player_id);

    if (state->phase == 2)
    {
        glColor3f(0.03f, 0.05f, 0.09f);
        draw_rect(180.0f, 190.0f, 440.0f, 124.0f);
        glColor3f(0.99f, 0.83f, 0.39f);
        draw_text(220.0f, 218.0f, 2.0f, "ROUND WINNER");

        char winners[96] = "";
        size_t used = 0;
        for (int i = 0; i < MAX_PLAYERS; ++i)
            if ((state->winners & (1 << i)) != 0)
            {
                int written = snprintf(winners + used, sizeof(winners) - used,
                                       "%sP%d", used ? " & " : "", i + 1);
                if (written > 0 && (size_t)written < sizeof(winners) - used)
                    used += (size_t)written;
            }
        if (used == 0)
            snprintf(winners, sizeof(winners), "No winner");
        glColor3f(0.91f, 0.95f, 1.0f);
        draw_text(220.0f, 260.0f, 1.8f, winners);
        snprintf(line, sizeof(line), "BEST SCORE  %d", state->best_score);
        glColor3f(0.67f, 0.77f, 0.89f);
        draw_text(420.0f, 268.0f, 1.25f, line);
    }

    /* Live score cards always come from the latest server snapshot. */
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (!state->player_active[i])
            continue;
        snprintf(line, sizeof(line), "P%d  %d pts%s", i + 1, state->player_score[i],
                 player_id == i + 1 ? "  YOU" : "");
        glColor3f(player_id == i + 1 ? 0.61f : 0.75f,
                  player_id == i + 1 ? 0.89f : 0.82f,
                  0.97f);
        draw_text(24.0f + (float)i * 148.0f, (float)ARENA_H - 22.0f, 1.15f, line);
    }
}

static void *dds_worker(void *unused)
{
    (void)unused;
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0)
    {
        fprintf(stderr, "Could not create DDS participant: %s\n", dds_strretcode(-participant));
        return NULL;
    }

    dds_entity_t join_topic = dds_create_topic(participant, &JoinRequest_desc, "JoinRequest", NULL, NULL);
    dds_entity_t response_topic = dds_create_topic(participant, &JoinResponse_desc, "JoinResponse", NULL, NULL);
    dds_entity_t input_topic = dds_create_topic(participant, &Input_desc, "Input", NULL, NULL);
    dds_entity_t state_topic = dds_create_topic(participant, &GameState_desc, "GameState", NULL, NULL);
    dds_entity_t response_reader = dds_create_reader(participant, response_topic, NULL, NULL);
    dds_entity_t state_reader = dds_create_reader(participant, state_topic, NULL, NULL);
    dds_entity_t join_writer = dds_create_writer(participant, join_topic, NULL, NULL);
    dds_entity_t input_writer = dds_create_writer(participant, input_topic, NULL, NULL);

    if (join_topic < 0 || response_topic < 0 || input_topic < 0 || state_topic < 0 ||
        response_reader < 0 || state_reader < 0 || join_writer < 0 || input_writer < 0)
    {
        fprintf(stderr, "Could not create DDS topics/readers/writers. Check the Cyclone DDS setup.\n");
        dds_delete(participant);
        return NULL;
    }

    void *response_samples[MAX_SAMPLES];
    void *state_samples[MAX_SAMPLES];
    response_samples[0] = JoinResponse__alloc();
    state_samples[0] = GameState__alloc();
    if (response_samples[0] == NULL || state_samples[0] == NULL)
    {
        fprintf(stderr, "Could not allocate DDS samples.\n");
        dds_delete(participant);
        return NULL;
    }
    dds_sample_info_t response_info[MAX_SAMPLES];
    dds_sample_info_t state_info[MAX_SAMPLES];
    uint64_t client_token = dds_time();
    if (client_token == 0)
        client_token = 1;
    JoinRequest request = {0};
    request.client_token = client_token;
    uint64_t last_join_request = 0;
    int player_id = 0;

    while (1)
    {
        pthread_mutex_lock(&g_mutex);
        bool running = g_client.running;
        char direction = g_client.direction;
        pthread_mutex_unlock(&g_mutex);
        if (!running)
            break;

        if (player_id == 0)
        {
            dds_return_t rc = dds_take(response_reader, response_samples, response_info, 1, 1);
            if (rc < 0)
                fprintf(stderr, "DDS join response read failed: %s\n", dds_strretcode(-rc));
            if (rc > 0 && response_info[0].valid_data)
            {
                JoinResponse *response = (JoinResponse *)response_samples[0];
                if (response->client_token == client_token &&
                    response->int_player_id > 0 && response->int_player_id <= MAX_PLAYERS)
                {
                    player_id = (int)response->int_player_id;
                    pthread_mutex_lock(&g_mutex);
                    g_client.player_id = player_id;
                    pthread_mutex_unlock(&g_mutex);
                    printf("Joined Coin Rush as player %d\n", player_id);
                }
            }

            uint64_t now = dds_time();
            if (player_id == 0 && now - last_join_request >= DDS_SECS(1))
            {
                last_join_request = now;
                dds_write(join_writer, &request);
            }
        }
        else
        {
            Input input = {0};
            input.int_player_id = player_id;
            input.ch_direction = direction;
            dds_write(input_writer, &input);
        }

        dds_return_t rc = dds_take(state_reader, state_samples, state_info, 1, 1);
        if (rc > 0 && state_info[0].valid_data)
        {
            GameState *incoming = (GameState *)state_samples[0];
            pthread_mutex_lock(&g_mutex);
            g_client.state = *incoming;
            g_client.has_state = true;
            pthread_mutex_unlock(&g_mutex);
        }
        else if (rc < 0)
            fprintf(stderr, "DDS game state read failed: %s\n", dds_strretcode(-rc));

        dds_sleepfor(DDS_MSECS(10));
    }

    dds_free(response_samples[0]);
    dds_free(state_samples[0]);
    dds_delete(participant);
    return NULL;
}

int main(void)
{
    configure_wslg_renderer();
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
    {
        fprintf(stderr, "GLFW could not initialize.\n");
        return 1;
    }

    /* The renderer uses compatibility OpenGL calls (glBegin, glOrtho and
     * client-side vertex arrays), so request a compatibility profile. */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
    GLFWwindow *window = glfwCreateWindow(ARENA_W, ARENA_H, "Coin Rush | Player Client", NULL, NULL);
    if (!window)
    {
        fprintf(stderr, "Could not open the Coin Rush window.\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    glViewport(0, 0, framebuffer_width, framebuffer_height);
    glDrawBuffer(GL_BACK);
    glReadBuffer(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    const GLubyte *gl_version = glGetString(GL_VERSION);
    const GLubyte *gl_renderer = glGetString(GL_RENDERER);
    fprintf(stderr, "OpenGL context: %s | %s\n",
            gl_version ? (const char *)gl_version : "unavailable",
            gl_renderer ? (const char *)gl_renderer : "unknown renderer");
    fprintf(stderr, "Framebuffer: %d x %d\n", framebuffer_width, framebuffer_height);
    GLint draw_buffer = 0;
    GLint read_buffer = 0;
    GLint double_buffered = 0;
    glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_DOUBLEBUFFER, &double_buffered);
    fprintf(stderr, "OpenGL buffers: draw=0x%x read=0x%x double=%d\n",
            (unsigned int)draw_buffer, (unsigned int)read_buffer, double_buffered);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, ARENA_W, ARENA_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    pthread_mutex_lock(&g_mutex);
    g_client.running = true;
    g_client.direction = 'n';
    g_client.player_id = 0;
    pthread_mutex_unlock(&g_mutex);

    pthread_t worker;
    if (pthread_create(&worker, NULL, dds_worker, NULL) != 0)
    {
        fprintf(stderr, "Could not start the DDS worker thread.\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    bool gl_error_reported = false;
    bool render_probe_reported = false;
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        char direction = 'n';
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            direction = 'l';
        else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            direction = 'r';

        pthread_mutex_lock(&g_mutex);
        g_client.direction = direction;
        GameState state = g_client.state;
        bool has_state = g_client.has_state;
        int player_id = g_client.player_id;
        pthread_mutex_unlock(&g_mutex);

        glLoadIdentity();
        draw_arena(&state, has_state, player_id);
        GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR && !gl_error_reported)
        {
            fprintf(stderr, "OpenGL draw error: 0x%04x\n", (unsigned int)gl_error);
            gl_error_reported = true;
        }
        if (!render_probe_reported)
        {
            unsigned char pixel[3] = {0, 0, 0};
            glFinish();
            glReadPixels(10, framebuffer_height - 10, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, pixel);
            fprintf(stderr, "Top-left render probe RGB: %u, %u, %u\n",
                    (unsigned int)pixel[0], (unsigned int)pixel[1], (unsigned int)pixel[2]);
            GLenum read_error = glGetError();
            if (read_error != GL_NO_ERROR)
                fprintf(stderr, "OpenGL readback error: 0x%04x\n", (unsigned int)read_error);
            render_probe_reported = true;
        }
        glfwSwapBuffers(window);
    }

    pthread_mutex_lock(&g_mutex);
    g_client.running = false;
    pthread_mutex_unlock(&g_mutex);
    pthread_join(worker, NULL);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
