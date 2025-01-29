#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/glew.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_thread.h>

#include "device.h"
#include "input-event-codes.h"
#include "virtio.h"
#include "window.h"

#define SDL_COND_TIMEOUT 1 /* ms */

#define DEF_KEY_MAP(_sdl_key, _linux_key)            \
    {                                                \
        .sdl_key = _sdl_key, .linux_key = _linux_key \
    }

struct key_map_entry {
    int sdl_key;
    int linux_key;
};

struct key_map_entry key_map[] = {
    /* Mouse */
    DEF_KEY_MAP(SDL_BUTTON_LEFT, BTN_LEFT),
    DEF_KEY_MAP(SDL_BUTTON_RIGHT, BTN_RIGHT),
    DEF_KEY_MAP(SDL_BUTTON_MIDDLE, BTN_MIDDLE),
    /* Keyboard */
    DEF_KEY_MAP(SDLK_ESCAPE, KEY_ESC),
    DEF_KEY_MAP(SDLK_1, KEY_1),
    DEF_KEY_MAP(SDLK_2, KEY_2),
    DEF_KEY_MAP(SDLK_3, KEY_3),
    DEF_KEY_MAP(SDLK_4, KEY_4),
    DEF_KEY_MAP(SDLK_5, KEY_5),
    DEF_KEY_MAP(SDLK_6, KEY_6),
    DEF_KEY_MAP(SDLK_7, KEY_7),
    DEF_KEY_MAP(SDLK_8, KEY_8),
    DEF_KEY_MAP(SDLK_9, KEY_9),
    DEF_KEY_MAP(SDLK_0, KEY_0),
    DEF_KEY_MAP(SDLK_MINUS, KEY_MINUS),
    DEF_KEY_MAP(SDLK_EQUALS, KEY_EQUAL),
    DEF_KEY_MAP(SDLK_BACKSPACE, KEY_BACKSPACE),
    DEF_KEY_MAP(SDLK_TAB, KEY_TAB),
    DEF_KEY_MAP(SDLK_q, KEY_Q),
    DEF_KEY_MAP(SDLK_w, KEY_W),
    DEF_KEY_MAP(SDLK_e, KEY_E),
    DEF_KEY_MAP(SDLK_r, KEY_R),
    DEF_KEY_MAP(SDLK_t, KEY_T),
    DEF_KEY_MAP(SDLK_y, KEY_Y),
    DEF_KEY_MAP(SDLK_u, KEY_U),
    DEF_KEY_MAP(SDLK_i, KEY_I),
    DEF_KEY_MAP(SDLK_o, KEY_O),
    DEF_KEY_MAP(SDLK_p, KEY_P),
    DEF_KEY_MAP(SDLK_LEFTBRACKET, KEY_LEFTBRACE),
    DEF_KEY_MAP(SDLK_RIGHTBRACKET, KEY_RIGHTBRACE),
    DEF_KEY_MAP(SDLK_RETURN, KEY_ENTER),
    DEF_KEY_MAP(SDLK_LCTRL, KEY_LEFTCTRL),
    DEF_KEY_MAP(SDLK_a, KEY_A),
    DEF_KEY_MAP(SDLK_s, KEY_S),
    DEF_KEY_MAP(SDLK_d, KEY_D),
    DEF_KEY_MAP(SDLK_f, KEY_F),
    DEF_KEY_MAP(SDLK_g, KEY_G),
    DEF_KEY_MAP(SDLK_h, KEY_H),
    DEF_KEY_MAP(SDLK_j, KEY_J),
    DEF_KEY_MAP(SDLK_k, KEY_K),
    DEF_KEY_MAP(SDLK_l, KEY_L),
    DEF_KEY_MAP(SDLK_SEMICOLON, KEY_SEMICOLON),
    DEF_KEY_MAP(SDLK_BACKQUOTE, KEY_GRAVE),
    DEF_KEY_MAP(SDLK_LSHIFT, KEY_LEFTSHIFT),
    DEF_KEY_MAP(SDLK_BACKSLASH, KEY_BACKSLASH),
    DEF_KEY_MAP(SDLK_z, KEY_Z),
    DEF_KEY_MAP(SDLK_x, KEY_X),
    DEF_KEY_MAP(SDLK_c, KEY_C),
    DEF_KEY_MAP(SDLK_v, KEY_V),
    DEF_KEY_MAP(SDLK_b, KEY_B),
    DEF_KEY_MAP(SDLK_n, KEY_N),
    DEF_KEY_MAP(SDLK_m, KEY_M),
    DEF_KEY_MAP(SDLK_COMMA, KEY_COMMA),
    DEF_KEY_MAP(SDLK_PERIOD, KEY_DOT),
    DEF_KEY_MAP(SDLK_SLASH, KEY_SLASH),
    DEF_KEY_MAP(SDLK_RSHIFT, KEY_RIGHTSHIFT),
    DEF_KEY_MAP(SDLK_LALT, KEY_LEFTALT),
    DEF_KEY_MAP(SDLK_SPACE, KEY_SPACE),
    DEF_KEY_MAP(SDLK_CAPSLOCK, KEY_CAPSLOCK),
    DEF_KEY_MAP(SDLK_F1, KEY_F1),
    DEF_KEY_MAP(SDLK_F2, KEY_F2),
    DEF_KEY_MAP(SDLK_F3, KEY_F3),
    DEF_KEY_MAP(SDLK_F4, KEY_F4),
    DEF_KEY_MAP(SDLK_F5, KEY_F5),
    DEF_KEY_MAP(SDLK_F6, KEY_F6),
    DEF_KEY_MAP(SDLK_F7, KEY_F7),
    DEF_KEY_MAP(SDLK_F7, KEY_F8),
    DEF_KEY_MAP(SDLK_F9, KEY_F9),
    DEF_KEY_MAP(SDLK_F10, KEY_F10),
    DEF_KEY_MAP(SDLK_SCROLLLOCK, KEY_SCROLLLOCK),
    DEF_KEY_MAP(SDLK_KP_7, KEY_KP7),
    DEF_KEY_MAP(SDLK_KP_8, KEY_KP8),
    DEF_KEY_MAP(SDLK_KP_9, KEY_KP9),
    DEF_KEY_MAP(SDLK_KP_MINUS, KEY_KPMINUS),
    DEF_KEY_MAP(SDLK_KP_4, KEY_KP4),
    DEF_KEY_MAP(SDLK_KP_5, KEY_KP5),
    DEF_KEY_MAP(SDLK_KP_6, KEY_KP6),
    DEF_KEY_MAP(SDLK_KP_PLUS, KEY_KPPLUS),
    DEF_KEY_MAP(SDLK_KP_1, KEY_KP1),
    DEF_KEY_MAP(SDLK_KP_2, KEY_KP2),
    DEF_KEY_MAP(SDLK_KP_3, KEY_KP3),
    DEF_KEY_MAP(SDLK_KP_0, KEY_KP0),
    DEF_KEY_MAP(SDLK_KP_PERIOD, KEY_KPDOT),
    DEF_KEY_MAP(SDLK_F11, KEY_F11),
    DEF_KEY_MAP(SDLK_F12, KEY_F12),
    DEF_KEY_MAP(SDLK_KP_ENTER, KEY_KPENTER),
    DEF_KEY_MAP(SDLK_RCTRL, KEY_RIGHTCTRL),
    DEF_KEY_MAP(SDLK_RALT, KEY_RIGHTALT),
    DEF_KEY_MAP(SDLK_HOME, KEY_HOME),
    DEF_KEY_MAP(SDLK_UP, KEY_UP),
    DEF_KEY_MAP(SDLK_PAGEUP, KEY_PAGEUP),
    DEF_KEY_MAP(SDLK_LEFT, KEY_LEFT),
    DEF_KEY_MAP(SDLK_RIGHT, KEY_RIGHT),
    DEF_KEY_MAP(SDLK_END, KEY_END),
    DEF_KEY_MAP(SDLK_DOWN, KEY_DOWN),
    DEF_KEY_MAP(SDLK_PAGEDOWN, KEY_PAGEDOWN),
    DEF_KEY_MAP(SDLK_INSERT, KEY_INSERT),
    DEF_KEY_MAP(SDLK_DELETE, KEY_DELETE),
};

struct display_info {
    /* Request type: primary or cursor */
    int render_type;

    /* Primary plane */
    struct gpu_resource resource;
    uint32_t primary_sdl_format;
    SDL_Texture *primary_texture;

    /* Cursor plane */
    struct gpu_resource cursor;
    uint32_t cursor_sdl_format;
    uint32_t *cursor_img;
    SDL_Rect cursor_rect; /* Cursor size and position */
    SDL_Texture *cursor_texture;

    SDL_mutex *img_mtx;
    SDL_cond *img_cond;
    SDL_Thread *win_thread;
    SDL_Thread *ev_thread;
    SDL_Window *window;
    SDL_Renderer *renderer;

#if SEMU_HAS(VIRGL)
    SDL_GLContext win_ctx;
    GLuint gl_fb;
    GLuint gl_texture;
#endif
};

static struct display_info displays[VIRTIO_GPU_MAX_SCANOUTS];
static int display_cnt;

void window_add(uint32_t width, uint32_t height)
{
    displays[display_cnt].resource.width = width;
    displays[display_cnt].resource.height = height;
    display_cnt++;
}

static int sdl_key_to_linux_key(int sdl_key)
{
    unsigned long key_cnt = sizeof(key_map) / sizeof(struct key_map_entry);
    for (unsigned long i = 0; i < key_cnt; i++)
        if (sdl_key == key_map[i].sdl_key)
            return key_map[i].linux_key;

    return -1;
}

static int event_thread(void *data)
{
    int linux_key;

    while (1) {
        SDL_Event e;
        if (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: {
                exit(0);
            }
            case SDL_KEYDOWN: {
                linux_key = sdl_key_to_linux_key(e.key.keysym.sym);
                virtio_input_update_key(linux_key, 1);
                break;
            }
            case SDL_KEYUP: {
                linux_key = sdl_key_to_linux_key(e.key.keysym.sym);
                virtio_input_update_key(linux_key, 0);
                break;
            }
            case SDL_MOUSEBUTTONDOWN: {
                linux_key = sdl_key_to_linux_key(e.button.button);
                virtio_input_update_mouse_button_state(linux_key, true);
                break;
            }
            case SDL_MOUSEBUTTONUP: {
                linux_key = sdl_key_to_linux_key(e.button.button);
                virtio_input_update_mouse_button_state(linux_key, false);
                break;
            }
            case SDL_MOUSEMOTION: {
                virtio_input_update_cursor(e.motion.x, e.motion.y);
                break;
            }
            }
        }
    }
}

#if !SEMU_HAS(VIRGL)
static int window_thread(void *data)
{
    struct display_info *display = (struct display_info *) data;
    struct gpu_resource *resource = &display->resource;
    struct gpu_resource *cursor = &display->cursor;

    /* Create SDL window */
    display->window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, resource->width,
                                       resource->height, SDL_WINDOW_SHOWN);

    if (!display->window) {
        fprintf(stderr, "%s(): failed to create window\n", __func__);
        exit(2);
    }

    /* Create SDL render */
    display->renderer =
        SDL_CreateRenderer(display->window, -1, SDL_RENDERER_ACCELERATED);

    if (!display->renderer) {
        fprintf(stderr, "%s(): failed to create renderer\n", __func__);
        exit(2);
    }

    /* Render the whole screen with black color */
    SDL_SetRenderDrawColor(display->renderer, 0, 0, 0, 255);
    SDL_RenderClear(display->renderer);
    SDL_RenderPresent(display->renderer);

    /* Create event handling thread */
    ((struct display_info *) data)->ev_thread =
        SDL_CreateThread(event_thread, NULL, data);

    SDL_Surface *surface;

    while (1) {
        /* Mutex lock */
        SDL_LockMutex(display->img_mtx);

        /* Wait until the image is arrived */
        while (SDL_CondWaitTimeout(display->img_cond, display->img_mtx,
                                   SDL_COND_TIMEOUT))
            ;

        if (display->render_type == RENDER_PRIMARY_PLANE) {
            /* Generate primary plane texture */
            surface = SDL_CreateRGBSurfaceWithFormatFrom(
                resource->image, resource->width, resource->height,
                resource->bits_per_pixel, resource->stride,
                display->primary_sdl_format);

            SDL_DestroyTexture(display->primary_texture);
            display->primary_texture =
                SDL_CreateTextureFromSurface(display->renderer, surface);
            SDL_FreeSurface(surface);
        } else if (display->render_type == UPDATE_CURSOR_RESOURCE) {
            /* Generate cursor plane texture */
            surface = SDL_CreateRGBSurfaceWithFormatFrom(
                cursor->image, cursor->width, cursor->height, CURSOR_BPP,
                CURSOR_STRIDE, SDL_PIXELFORMAT_ARGB8888);

            SDL_DestroyTexture(display->cursor_texture);
            display->cursor_texture =
                SDL_CreateTextureFromSurface(display->renderer, surface);
            SDL_FreeSurface(surface);
        } else if (display->render_type == CLEAR_CURSOR_RESOURCE) {
            SDL_DestroyTexture(display->cursor_texture);
            display->cursor_texture = NULL;
        }

        /* Render primary and cursor planes */
        SDL_RenderClear(display->renderer);

        if (display->primary_texture)
            SDL_RenderCopy(display->renderer, display->primary_texture, NULL,
                           NULL);

        if (display->cursor_texture)
            SDL_RenderCopy(display->renderer, display->cursor_texture, NULL,
                           &display->cursor_rect);

        SDL_RenderPresent(display->renderer);

        /* Mutex unlock */
        SDL_UnlockMutex(display->img_mtx);
    }
}
#else
void clear_screen_gl(struct display_info *display)
{
    /* Set window for GL to work */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Set rendering region */
    int width = 0, height = 0;
    SDL_GetWindowSize(display->window, &width, &height);
    glViewport(0, 0, width, height);

    /* Generate GL texture */
    uint32_t gl_texture;
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &gl_texture);
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Fill GL texture with black */
    glColor3f(0.0f, /*0.0f*/ 255, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1.0f, -1.0f);  // buttom left
    glVertex2f(1.0f, -1.0f);   // buttom right
    glVertex2f(1.0f, 1.0f);    // upper right
    glVertex2f(-1.0f, 1.0f);   // upper left
    glEnd();

    /* Update the window */
    SDL_GL_SwapWindow(display->window);

    /* Delete GL texture */
    glDeleteTextures(1, &gl_texture);
}

void setup_scanout_gl(int scanout_id, uint32_t texture_id)
{
    struct display_info *display = &displays[scanout_id];

    /* Set window for GL to work */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Allocate GL framebuffer for the guest */
    if (!display->gl_fb) {
        glGenFramebuffers(1, &display->gl_fb);
        printf("@@@ Framebuffer created = %d\n", display->gl_fb);
    }

    display->gl_texture = texture_id;

    /* Setup framebuffer for the guest */
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, display->gl_fb);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, display->gl_texture, 0);
}

void flush_scanout_gl(struct display_info *display)
{
    /* Set window for GL to work */
    SDL_GL_MakeCurrent(display->window, display->win_ctx);

    /* Set rendering region */
    int width = 0, height = 0;
    SDL_GetWindowSize(display->window, &width, &height);
    glViewport(0, 0, width, height);

    printf("###USING %d\n", display->gl_fb);

#if 1
    /* Generate GL texture */
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &display->gl_texture);
    glBindTexture(GL_TEXTURE_2D, display->gl_texture);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Fill GL texture with black */
    glColor3f(255.0f, 0.0f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1.0f, -1.0f);  // buttom left
    glVertex2f(1.0f, -1.0f);   // buttom right
    glVertex2f(1.0f, 1.0f);    // upper right
    glVertex2f(-1.0f, 1.0f);   // upper left
    glEnd();

    glBindFramebuffer(GL_FRAMEBUFFER_EXT, display->gl_fb);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, display->gl_texture, 0);
#endif

    /* Specifying source and destination of framebuffer copy */
    uint32_t src_fb = display->gl_fb; /* Guest framebuffer */
    uint32_t dst_fb = 0;              /* 0 to write to the window */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fb);

    /* Source */
    GLint src_x0 = 0, src_y0 = 0;
    GLint src_x1 = width, src_y1 = height;

    /* Destination */
    GLint dst_x0 = 0, dst_y0 = 0;
    GLint dst_x1 = width, dst_y1 = height;

    /* Copy framebuffer from guest to the window */
    glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1,
                      dst_y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* Update the window */
    SDL_GL_SwapWindow(display->window);
}

static int window_thread_gl(void *data)
{
    struct display_info *display = (struct display_info *) data;
    struct gpu_resource *resource = &display->resource;

    /* Create SDL window */
    display->window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, resource->width,
                                       resource->height,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (!display->window) {
        fprintf(stderr, "%s(): failed to create window\n", __func__);
        exit(2);
    }

    /* Initialize window context */
    display->win_ctx = SDL_GL_CreateContext(display->window);
    clear_screen_gl(display);

    /* Create event handling thread */
    ((struct display_info *) data)->ev_thread =
        SDL_CreateThread(event_thread, NULL, data);

    while (1) {
        /* Mutex lock */
        SDL_LockMutex(display->img_mtx);

        /* Wait until the image is arrived */
        while (SDL_CondWaitTimeout(display->img_cond, display->img_mtx,
                                   SDL_COND_TIMEOUT))
            ;

        if (display->render_type == RENDER_PRIMARY_PLANE) {
            printf("!!!RENDER_PRIMARY_PLANE!!!\n");
            flush_scanout_gl(display);
        } else if (display->render_type == UPDATE_CURSOR_RESOURCE) {
        } else if (display->render_type == CLEAR_CURSOR_RESOURCE) {
        }

        /* Mutex unlock */
        SDL_UnlockMutex(display->img_mtx);
    }
}
#endif

void window_init(void)
{
    glewInit();

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "%s(): failed to initialize SDL\n", __func__);
        exit(2);
    }

    for (int i = 0; i < display_cnt; i++) {
        displays[i].img_mtx = SDL_CreateMutex();
        displays[i].img_cond = SDL_CreateCond();

#if !SEMU_HAS(VIRGL)
        displays[i].win_thread =
            SDL_CreateThread(window_thread, NULL, (void *) &displays[i]);
#else
        displays[i].win_thread =
            SDL_CreateThread(window_thread_gl, NULL, (void *) &displays[i]);
#endif
        SDL_DetachThread(displays[i].win_thread);
    }
}

void window_lock(uint32_t id)
{
    SDL_LockMutex(displays[id].img_mtx);
}

void window_unlock(uint32_t id)
{
    SDL_UnlockMutex(displays[id].img_mtx);
}

static bool virtio_gpu_to_sdl_format(uint32_t virtio_gpu_format,
                                     uint32_t *sdl_format)
{
    switch (virtio_gpu_format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ARGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XRGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRA8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRX8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ABGR8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBX8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBA8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XBGR8888;
        return true;
    default:
        return false;
    }
}

void cursor_clear(int scanout_id)
{
    /* Reset cursor information */
    struct display_info *display = &displays[scanout_id];
    memset(&display->cursor_rect, 0, sizeof(SDL_Rect));
    display->cursor_sdl_format = 0;

    /* Reset cursor resource */
    memset(&display->cursor, 0, sizeof(struct gpu_resource));
    free(display->cursor_img);
    display->cursor_img = NULL;
    display->cursor.image = NULL;

    /* Trigger plane rendering */
    display->render_type = CLEAR_CURSOR_RESOURCE;
    SDL_CondSignal(display->img_cond);
}

void cursor_update(struct gpu_resource *resource, int scanout_id, int x, int y)
{
    /* Convert virtio-gpu resource format to SDL format */
    uint32_t sdl_format;
    bool legal_format = virtio_gpu_to_sdl_format(resource->format, &sdl_format);

    if (!legal_format) {
        fprintf(stderr, "Invalid resource format.\n");
        return;
    }

    /* Update cursor information */
    struct display_info *display = &displays[scanout_id];
    display->cursor_rect.x = x;
    display->cursor_rect.y = y;
    display->cursor_rect.w = resource->width;
    display->cursor_rect.h = resource->height;
    display->cursor_sdl_format = sdl_format;

    /* Cursor resource update */
    memcpy(&display->cursor, resource, sizeof(struct gpu_resource));
    size_t pixels_size = sizeof(uint32_t) * resource->width * resource->height;
    free(display->cursor_img);
    display->cursor_img = malloc(pixels_size);
    display->cursor.image = display->cursor_img;
    memcpy(display->cursor_img, resource->image, pixels_size);

    /* Trigger cursor rendering */
    display->render_type = UPDATE_CURSOR_RESOURCE;
    SDL_CondSignal(display->img_cond);
}

void cursor_move(int scanout_id, int x, int y)
{
    /* Update cursor position */
    struct display_info *display = &displays[scanout_id];
    display->cursor_rect.x = x;
    display->cursor_rect.y = y;

    /* Trigger cursor rendering */
    display->render_type = MOVE_CURSOR_POSITION;
    SDL_CondSignal(display->img_cond);
}

void window_render(struct gpu_resource *resource)
{
    int id = resource->scanout_id;
    struct display_info *display = &displays[id];

    /* Convert virtio-gpu resource format to SDL format */
    uint32_t sdl_format;
    bool legal_format = virtio_gpu_to_sdl_format(resource->format, &sdl_format);

    if (!legal_format) {
        fprintf(stderr, "Invalid resource format.\n");
        return;
    }

    /* Update primary plane resource */
    display->primary_sdl_format = sdl_format;
    memcpy(&display->resource, resource, sizeof(struct gpu_resource));

    /* Trigger primary plane rendering */
    display->render_type = RENDER_PRIMARY_PLANE;
    SDL_CondSignal(display->img_cond);
}

void window_render_gl(int scanout_id)
{
    /* Trigger primary plane rendering */
    struct display_info *display = &displays[scanout_id];
    display->render_type = RENDER_PRIMARY_PLANE;
    SDL_CondSignal(display->img_cond);
}
