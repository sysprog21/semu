/* clang-format off */
/* epoxy/gl.h must be included before GL/gl.h */
#include <epoxy/gl.h>
/* clang-format on */
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_thread.h>
#include <virglrenderer.h>

#include "device.h"
#include "window.h"

#define SDL_COND_TIMEOUT 1 /* ms */
#define WINDOW_SCREEN_FB 0

struct display_info {
    /* Primary plane */
    int width;
    int height;
    GLuint primary_fb;
    GLuint primary_texture;

    /* Cursor plane */
    GLuint cursor_texture;

    /* Display window */
    SDL_Thread *win_thread;
    SDL_Thread *ev_thread;
    SDL_Window *window;
    SDL_GLContext win_ctx;
};

static struct display_info displays[VIRTIO_GPU_MAX_SCANOUTS];
static int display_cnt;

/* Callback function of create_gl_context() for virglrender */
virgl_renderer_gl_context sdl_create_context(
    int scanout_id,
    struct virgl_renderer_gl_ctx_param *params)
{
    struct display_info *display = &displays[scanout_id];

    /* Select GL context to work on */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Set GL attributes */
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);

    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver)) {
        fprintf(stderr, "%s(): set major version failed %d\n", __func__,
                params->major_ver);
        return NULL;
    }

    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, params->minor_ver)) {
        fprintf(stderr, "%s(): set minor version failed %d\n", __func__,
                params->minor_ver);
        return NULL;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(display->window);
    if (!ctx) {
        fprintf(stderr, "%s(): failed to create GL context: %s\n", __func__,
                SDL_GetError());
        return NULL;
    }

    return (void *) ctx;
}

/* Callback function of destroy_gl_context() for virglrender */
void sdl_destroy_context(SDL_GLContext ctx)
{
    SDL_GL_DeleteContext(ctx);
}

/* Callback function of virgl_make_context_current() for virglrender */
int sdl_make_context_current(int scanout_id, virgl_renderer_gl_context ctx)
{
    struct display_info *display = &displays[scanout_id];
    return SDL_GL_MakeCurrent(display->window, ctx);
}

static void window_add_gl(uint32_t width, uint32_t height)
{
    if (display_cnt >= VIRTIO_GPU_MAX_SCANOUTS) {
        fprintf(stderr, "%s(): display count exceeds maxiumum\n", __func__);
        exit(2);
    }

    displays[display_cnt].width = width;
    displays[display_cnt].height = height;
    display_cnt++;
}

static void window_clear_gl(int scanout_id)
{
    struct display_info *display = &displays[scanout_id];

    /* Select GL context to work on */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Get window size */
    int width = 0, height = 0;
    SDL_GetWindowSize(display->window, &width, &height);

    /* Set rendering region */
    glViewport(0, 0, width, height);

    /* Clear GL context */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Swap buffer to display the new context */
    SDL_GL_SwapWindow(display->window);
}

static void window_set_scanout_gl(int scanout_id, uint32_t texture_id)
{
    struct display_info *display = &displays[scanout_id];
    display->primary_texture = texture_id;

    /* Select GL context to work on */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Allocate GL framebuffer for the guest */
    if (!display->primary_fb)
        glGenFramebuffers(1, &display->primary_fb);

    /* Bind GL texture from virglrender with the framebuffer */
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, display->primary_fb);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, display->primary_texture, 0);
}

static void window_flush_gl(int scanout_id, int res_id)
{
    (void) res_id;
    struct display_info *display = &displays[scanout_id];

    /* Select GL context to work on */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Get window size */
    int width = 0, height = 0;
    SDL_GetWindowSize(display->window, &width, &height);

    /* Set rendering region */
    glViewport(0, 0, width, height);

    /* Set image blit region */
    GLint src_x0 = 0, src_y0 = 0;
    GLint src_x1 = width, src_y1 = height;
    GLint dst_x0 = 0, dst_y0 = 0;
    GLint dst_x1 = width, dst_y1 = height;

    /* Copy primary plane */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, display->primary_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, WINDOW_SCREEN_FB);
    glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1,
                      dst_y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* Update the window */
    SDL_GL_SwapWindow(display->window);
}

static void cursor_move_gl(int scanout_id, int x, int y)
{
    struct display_info *display = &displays[scanout_id];

    /* Select GL context to work on */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Get window size */
    int width = 0, height = 0;
    SDL_GetWindowSize(display->window, &width, &height);
    glViewport(0, 0, width, height);

    /* Clear GL context */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Copy cursor plane */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, display->primary_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, WINDOW_SCREEN_FB);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* Set coordinate system for cursor update */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Set RGBA mode to blend cursor plane with primary plane */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);

    /* Render cursor image */
    glBindTexture(GL_TEXTURE_2D, display->cursor_texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(x, y);
    glTexCoord2f(1, 0);
    glVertex2f(x + CURSOR_WIDTH, y);
    glTexCoord2f(1, 1);
    glVertex2f(x + CURSOR_WIDTH, y + CURSOR_HEIGHT);
    glTexCoord2f(0, 1);
    glVertex2f(x, y + CURSOR_HEIGHT);
    glEnd();

    /* Clean up */
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    /* Swap buffer to display the new context */
    SDL_GL_SwapWindow(display->window);
}

static int cursor_update_gl(int scanout_id, int res_id, int x, int y)
{
    (void) x;
    (void) y;
    uint32_t *cursor_data;
    uint32_t width, height;
    struct display_info *display = &displays[scanout_id];

    /* Select GL context to work on */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Get cursor image from the virglrenderer */
    cursor_data = virgl_renderer_get_cursor_data(res_id, &width, &height);

    if (!cursor_data || !width || !height)
        return -1;

    /* Create new GL texture to store the cursor image */
    glGenTextures(1, &display->cursor_texture);
    glBindTexture(GL_TEXTURE_2D, display->cursor_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CURSOR_WIDTH, CURSOR_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, cursor_data);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Show the cursor on the screen */
    cursor_move_gl(scanout_id, x, y);

    return 0;
}

static void window_init_gl(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "%s(): failed to initialize SDL\n", __func__);
        exit(2);
    }

    for (int i = 0; i < display_cnt; i++) {
        struct display_info *display = &displays[i];

        /* Create SDL window */
        display->window = SDL_CreateWindow(
            "semu", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            display->width, display->height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

        if (!display->window) {
            fprintf(stderr, "%s(): failed to create window\n", __func__);
            exit(2);
        }

        /* Initialize window context */
        display->win_ctx = SDL_GL_CreateContext(display->window);
        window_clear_gl(i);

        /* Create event handling thread */
        display->ev_thread =
            SDL_CreateThread(window_events_thread, NULL, display);
    }
}

const struct window_backend g_window = {
    .window_init = window_init_gl,
    .window_add = window_add_gl,
    .window_set_scanout = window_set_scanout_gl,
    .window_clear = window_clear_gl,
    .window_flush = window_flush_gl,
    .cursor_update = cursor_update_gl,
    .cursor_move = cursor_move_gl,
};
