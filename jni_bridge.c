/*
 * JNI bridge for labwc Wayland compositor on Android.
 * Replaces labwc's main() — runs the compositor on a native thread,
 * controlled from Kotlin via WaylandBridge.nativeStart/nativeStop.
 */
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include "render/egl.h" /* for wlr_egl_destroy (private header) */
#include "render/allocator/shm.h" /* for wlr_shm_allocator_create */
#include "ahb_allocator.h"
#include "buffer_presenter.h"
#include "config/rcxml.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-server-core.h>

#define LOG_TAG "labwc-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Android bionic uses __assert2, not __assert_fail (glibc/musl).
 * libxcb's assert() calls expand to __assert_fail when cross-compiled
 * with autotools targeting linux-android. Provide a shim. */
void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func) {
    __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, "ASSERT FAILED: %s (%s:%u %s)", expr, file, line, func);
    abort();
}

/* labwc headers */
#include <time.h>
#include "labwc.h"
#include "config/rcxml.h"
#include "output.h"
#include "input/cursor.h"
#include "view.h"
#include "theme.h"
extern struct server server;

extern void server_init(void);
extern void server_start(void);
extern void server_finish(void);
extern void rcxml_read(const char *config_file);
extern void rcxml_finish(void);
extern void font_finish(void);
extern void session_environment_init(void);
extern void session_shutdown(void);
extern void menu_init(void);

static pthread_t compositor_thread;
static volatile int compositor_running = 0;
static char socket_path_buf[256] = "";

/* Global hooks for Android renderer/allocator injection into labwc server.c */
struct wlr_renderer *android_renderer = NULL;
struct wlr_allocator *android_allocator = NULL;

/**
 * Create a GLES2 renderer using Android's native EGL.
 * Returns NULL on failure (caller should fall back to pixman).
 */
static struct wlr_renderer *create_android_gles2_renderer(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return NULL;
    }
    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        LOGE("eglInitialize failed");
        return NULL;
    }
    LOGI("EGL %d.%d initialized", major, minor);

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOGE("eglBindAPI(EGL_OPENGL_ES_API) failed");
        eglTerminate(dpy);
        return NULL;
    }

    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, 0, /* surfaceless */
        EGL_NONE,
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(dpy, config_attribs, &config, 1, &num_configs) ||
            num_configs == 0) {
        LOGE("eglChooseConfig failed (num_configs=%d)", num_configs);
        eglTerminate(dpy);
        return NULL;
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        eglTerminate(dpy);
        return NULL;
    }

    /* Make context current (surfaceless) so wlroots can query GL state */
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        LOGE("eglMakeCurrent (surfaceless) failed: 0x%x", eglGetError());
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        return NULL;
    }

    LOGI("EGL context created, wrapping in wlroots...");

    /* Log EGL extensions for GPU client capability diagnostics */
    const char *dpy_exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (dpy_exts) {
        LOGI("EGL display extensions: %s", dpy_exts);
        LOGI("EGL_EXT_image_dma_buf_import: %s",
            strstr(dpy_exts, "EGL_EXT_image_dma_buf_import") ? "YES" : "NO");
        LOGI("EGL_ANDROID_image_native_buffer: %s",
            strstr(dpy_exts, "EGL_ANDROID_image_native_buffer") ? "YES" : "NO");
        LOGI("EGL_ANDROID_get_native_client_buffer: %s",
            strstr(dpy_exts, "EGL_ANDROID_get_native_client_buffer") ? "YES" : "NO");
    }

    struct wlr_egl *egl = wlr_egl_create_with_context(dpy, ctx);
    if (!egl) {
        LOGE("wlr_egl_create_with_context failed");
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        return NULL;
    }

    struct wlr_renderer *renderer = wlr_gles2_renderer_create(egl);
    if (!renderer) {
        LOGE("wlr_gles2_renderer_create failed");
        wlr_egl_destroy(egl);
        return NULL;
    }

    LOGI("GLES2 renderer created successfully");
    return renderer;
}

/* Keyboard focus tracking — reset in compositor_main() */
static bool focus_sent = false;
static int focus_log_count = 0;

/* ANativeWindow for rendering output to screen */
static ANativeWindow *g_window = NULL;
static pthread_mutex_t g_window_lock = PTHREAD_MUTEX_INITIALIZER;

/* Zero-copy presenter (API 29+) — NULL means fallback to CPU blit */
static buffer_presenter_t *g_presenter = NULL;

/* Cached last frame for re-blit on surface recreation */
static void *g_last_frame = NULL;
static int g_last_frame_w = 0, g_last_frame_h = 0;
static size_t g_last_frame_stride = 0;

/* Viewport offset for pan (in compositor buffer pixels) */
static volatile int g_viewport_x = 0;
static volatile int g_viewport_y = 0;

/* Output commit listener — blits pixman buffer to ANativeWindow */
static struct wl_listener output_commit_listener;

static void on_output_commit(struct wl_listener *listener, void *data) {
    struct wlr_output_event_commit *event = data;
    const struct wlr_output_state *state = event->state;

    /* Only present when a buffer was committed */
    if (!(state->committed & WLR_OUTPUT_STATE_BUFFER) || !state->buffer) {
        static int skip_count = 0;
        if (skip_count++ % 60 == 0)
            LOGI("on_output_commit: no buffer (committed=0x%x skip=%d)", state->committed, skip_count);
        return;
    }

    /* Zero-copy path: submit AHardwareBuffer directly to SurfaceFlinger */
    if (g_presenter) {
        struct wlr_ahb_buffer *ahb_buf = try_get_ahb_buffer_from_buffer(state->buffer);
        if (ahb_buf && ahb_buf->ahb) {
            buffer_presenter_send(g_presenter, ahb_buf->ahb);
            static int present_count = 0;
            if (present_count++ % 300 == 0)
                LOGI("Zero-copy present %dx%d (count=%d)",
                     state->buffer->width, state->buffer->height, present_count);
            return;
        }
    }

    /* Fallback: CPU blit (API < 29 or non-AHB buffer) */
    pthread_mutex_lock(&g_window_lock);
    if (!g_window) {
        static int no_window_count = 0;
        if (no_window_count++ % 60 == 0)
            LOGI("on_output_commit: no window (count=%d)", no_window_count);
        pthread_mutex_unlock(&g_window_lock);
        return;
    }

    void *src_data = NULL;
    uint32_t src_format = 0;
    size_t src_stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(state->buffer,
            WLR_BUFFER_DATA_PTR_ACCESS_READ, &src_data, &src_format, &src_stride)) {
        LOGE("on_output_commit: begin_data_ptr_access failed");
        pthread_mutex_unlock(&g_window_lock);
        return;
    }

    ANativeWindow_Buffer nbuf;
    if (ANativeWindow_lock(g_window, &nbuf, NULL) == 0) {
        int src_w = (int)(src_stride / 4);
        int w = nbuf.width < src_w ? nbuf.width : src_w;
        int h = nbuf.height < state->buffer->height ? nbuf.height : state->buffer->height;
        size_t dst_stride = (size_t)nbuf.stride * 4;
        uint8_t *src = (uint8_t *)src_data;
        uint8_t *dst = (uint8_t *)nbuf.bits;
        memset(dst, 0, dst_stride * nbuf.height);
        for (int y = 0; y < h; y++) {
            memcpy(dst + y * dst_stride, src + y * src_stride, w * 4);
        }
        ANativeWindow_unlockAndPost(g_window);

        /* Cache frame for re-blit on surface recreation */
        size_t frame_size = src_stride * h;
        if (!g_last_frame || g_last_frame_w != w || g_last_frame_h != h) {
            free(g_last_frame);
            g_last_frame = malloc(frame_size);
        }
        if (g_last_frame) {
            memcpy(g_last_frame, src_data, frame_size);
            g_last_frame_w = w;
            g_last_frame_h = h;
            g_last_frame_stride = src_stride;
        }

        static int blit_count = 0;
        if (blit_count++ % 60 == 0)
            LOGI("CPU blit %dx%d (count=%d)", w, h, blit_count);
    }

    wlr_buffer_end_data_ptr_access(state->buffer);
    pthread_mutex_unlock(&g_window_lock);
}

static void android_wlr_log(enum wlr_log_importance importance,
                             const char *fmt, va_list args) {
    int prio = ANDROID_LOG_VERBOSE;
    switch (importance) {
    case WLR_ERROR: prio = ANDROID_LOG_ERROR; break;
    case WLR_INFO:  prio = ANDROID_LOG_INFO; break;
    case WLR_DEBUG: prio = ANDROID_LOG_DEBUG; break;
    default: break;
    }
    __android_log_vprint(prio, "wlroots", fmt, args);
}

/* Persistent theme storage — survives for the lifetime of the compositor thread */
static struct theme g_theme;

static struct wl_event_source *g_frame_timer = NULL;
static volatile int g_pending_resize_w = 0;
static volatile int g_pending_resize_h = 0;
static volatile int g_force_redraw = 0;

/* Pending view resize (keyboard open/close) — logical pixels */
static volatile int g_pending_view_w = 0;
static volatile int g_pending_view_h = 0;

/* Fixed Wayland output scale (integer, for protocol compliance) */
#define WAYLAND_SCALE 3

/* Maximum surface dimensions seen */
static int g_max_surface_w = 0;
static int g_max_surface_h = 0;

/* Zoom factor in permille (1000 = 1.0x, 2000 = 2.0x).
 * Higher precision than percent for smooth pinch-to-zoom. */
static volatile int g_zoom_permille = 1000;

/* Input event queue (JNI thread → compositor thread) */
#define INPUT_QUEUE_SIZE 256
struct input_event {
    enum { INPUT_NONE, INPUT_MOTION, INPUT_BUTTON, INPUT_KEY, INPUT_SCROLL } type;
    double x, y;        /* for motion: absolute coords 0..1; for scroll: axis value */
    uint32_t code;      /* button or keycode; for scroll: axis (0=vert, 1=horiz) */
    int pressed;         /* 1=press, 0=release */
};
static struct input_event g_input_queue[INPUT_QUEUE_SIZE];
static volatile int g_input_write = 0;
static volatile int g_input_read = 0;

static void queue_input(struct input_event ev) {
    int next = (g_input_write + 1) % INPUT_QUEUE_SIZE;
    if (next == g_input_read) return; /* full, drop */
    g_input_queue[g_input_write] = ev;
    g_input_write = next;
}

static int frame_timer_cb(void *data) {
    (void)data;

    /* Handle pending resize from JNI thread */
    int rw = g_pending_resize_w;
    int rh = g_pending_resize_h;
    if (rw > 0 && rh > 0) {
        g_pending_resize_w = 0;
        g_pending_resize_h = 0;
        /* Mode = physical pixels, fixed Wayland scale.
         * Pixman buffer is at mode size, clients see mode/scale logical. */
        struct output *lab_output;
        wl_list_for_each(lab_output, &server.outputs, link) {
            /* Force output re-enable with fresh mode. This ensures
             * the swapchain is recreated after a surface destruction
             * (tab switch away and back). */
            struct wlr_output_state ostate;
            wlr_output_state_init(&ostate);
            wlr_output_state_set_enabled(&ostate, true);
            wlr_output_state_set_custom_mode(&ostate, rw, rh, 0);
            wlr_output_state_set_scale(&ostate, WAYLAND_SCALE);
            if (!wlr_output_commit_state(lab_output->wlr_output, &ostate)) {
                LOGE("Output commit failed for %dx%d", rw, rh);
            }
            wlr_output_state_finish(&ostate);
            LOGI("Output %dx%d scale=%d (logical %dx%d)",
                 rw, rh, WAYLAND_SCALE, rw/WAYLAND_SCALE, rh/WAYLAND_SCALE);
            break;
        }
    }

    /* Handle pending view resize (keyboard open/close).
     * Resize all mapped views to fit the visible area. */
    int vw = g_pending_view_w;
    int vh = g_pending_view_h;
    if (vw > 0 && vh > 0) {
        g_pending_view_w = 0;
        g_pending_view_h = 0;
        struct view *view;
        wl_list_for_each(view, &server.views, link) {
            if (view->mapped && view->surface) {
                struct wlr_box geo = { .x = 0, .y = 0, .width = vw, .height = vh };
                view_move_resize(view, geo);
                LOGI("View resized to %dx%d", vw, vh);
            }
        }
    }

    /* Auto-maximize new views and ensure keyboard focus */
    {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat.wlr_seat);
        struct view *view;
        wl_list_for_each(view, &server.views, link) {
            if (view->mapped && view->surface) {
                /* Auto-maximize and unminimize — on mobile there's
                 * no taskbar to restore minimized windows. */
                if (view->minimized) {
                    view_minimize(view, false);
                    LOGI("View unminimized");
                }
                if (view->maximized != VIEW_AXIS_BOTH) {
                    view_maximize(view, VIEW_AXIS_BOTH);
                    LOGI("View auto-maximized");
                }
                /* Send keyboard focus on first mapped view */
                if (kb && !focus_sent) {
                    wlr_seat_keyboard_notify_enter(server.seat.wlr_seat,
                        view->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
                    focus_sent = true;
                    LOGI("Keyboard focus enter sent: surface=%p", view->surface);
                }
                break;
            }
        }
        if (focus_log_count++ % 300 == 0) {
            LOGI("Focus check: focused=%p kb=%p focus_sent=%d",
                 server.seat.wlr_seat->keyboard_state.focused_surface, kb, focus_sent);
        }
    }

    /* Process queued input events */
    while (g_input_read != g_input_write) {
        struct input_event ev = g_input_queue[g_input_read];
        g_input_read = (g_input_read + 1) % INPUT_QUEUE_SIZE;
        uint32_t now = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000));
        switch (ev.type) {
        case INPUT_MOTION: {
            wlr_cursor_warp_absolute(server.seat.cursor, NULL, ev.x, ev.y);
            cursor_update_focus();
            wlr_seat_pointer_notify_motion(server.seat.wlr_seat,
                now, server.seat.cursor->x, server.seat.cursor->y);
            wlr_seat_pointer_notify_frame(server.seat.wlr_seat);
            static int motion_log = 0;
            if (motion_log++ % 5 == 0)
                LOGI("Pointer: %.3f,%.3f → cursor %d,%d focus=%p",
                     ev.x, ev.y, (int)server.seat.cursor->x, (int)server.seat.cursor->y,
                     server.seat.wlr_seat->pointer_state.focused_surface);
            break;
        }
        case INPUT_BUTTON: {
            /* Route through labwc's cursor handler so SSD buttons
             * (close, maximize, minimize) are processed internally.
             * Only forward to client if labwc says to. */
            bool notify;
            if (ev.pressed) {
                notify = cursor_process_button_press(&server.seat,
                    ev.code, now);
            } else {
                notify = cursor_process_button_release(&server.seat,
                    ev.code, now);
            }
            if (notify) {
                wlr_seat_pointer_notify_button(server.seat.wlr_seat,
                    now, ev.code, ev.pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                             : WL_POINTER_BUTTON_STATE_RELEASED);
            }
            if (!ev.pressed) {
                cursor_finish_button_release(&server.seat, ev.code);
            }
            wlr_seat_pointer_notify_frame(server.seat.wlr_seat);
            break;
        }
        case INPUT_KEY: {
            struct wlr_keyboard *kb = &server.seat.keyboard_group->keyboard;
            /* Update xkb state so modifiers (Ctrl, Shift, Alt) are tracked.
             * xkb keycodes are evdev + 8. */
            xkb_state_update_key(kb->xkb_state, ev.code + 8,
                ev.pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
            /* Send the key event to the focused client */
            wlr_seat_keyboard_notify_key(server.seat.wlr_seat,
                now, ev.code, ev.pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                         : WL_KEYBOARD_KEY_STATE_RELEASED);
            /* Send updated modifiers after every key (handles Ctrl, Shift, etc.) */
            wlr_seat_keyboard_notify_modifiers(server.seat.wlr_seat,
                &(struct wlr_keyboard_modifiers){
                    .depressed = xkb_state_serialize_mods(kb->xkb_state, XKB_STATE_MODS_DEPRESSED),
                    .latched = xkb_state_serialize_mods(kb->xkb_state, XKB_STATE_MODS_LATCHED),
                    .locked = xkb_state_serialize_mods(kb->xkb_state, XKB_STATE_MODS_LOCKED),
                    .group = xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE),
                });
            break;
        }
        case INPUT_SCROLL: {
            uint32_t axis = ev.code; /* 0=vertical, 1=horizontal */
            double value = ev.x;    /* positive = down/right */
            wlr_seat_pointer_notify_axis(server.seat.wlr_seat,
                now, axis, value,
                (int32_t)(value * 120), /* discrete: 120 units per notch */
                WL_POINTER_AXIS_SOURCE_FINGER,
                WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            wlr_seat_pointer_notify_frame(server.seat.wlr_seat);
            break;
        }
        default:
            break;
        }
    }

    /* Render the scene and commit the output.
     * Send frame_done to all surfaces so clients continue rendering
     * (headless backend doesn't generate vsync-driven frame events). */
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    struct output *lab_output;
    wl_list_for_each(lab_output, &server.outputs, link) {
        if (lab_output->scene_output) {
            wlr_scene_output_send_frame_done(lab_output->scene_output, &now_ts);
            /* Force full damage so blit happens every frame.
             * Needed for viewport pan and visual zoom during pinch. */
            wlr_damage_ring_add_whole(&lab_output->scene_output->damage_ring);
            bool committed = wlr_scene_output_commit(lab_output->scene_output, NULL);
            static int commit_fail_count = 0;
            if (!committed) {
                if (commit_fail_count++ % 60 == 0)
                    LOGE("scene_output_commit returned false (count=%d)", commit_fail_count);
            } else {
                commit_fail_count = 0;
            }
        }
        break;
    }

    /* Flush all queued protocol events (input + frame_done) to clients */
    wl_display_flush_clients(server.wl_display);
    /* Always run at 33ms (~30fps). The idle 500ms interval caused missed
     * redraws when the surface was recreated during the sleep. */
    int interval = 33;
    if (g_frame_timer) {
        wl_event_source_timer_update(g_frame_timer, interval);
    }
    return 0;
}

static void *compositor_main(void *arg) {
    (void)arg;
    LOGI("Compositor thread starting");

    /* Reset state from any previous compositor run */
    focus_sent = false;
    focus_log_count = 0;
    g_input_read = 0;
    g_input_write = 0;

    wlr_log_init(WLR_DEBUG, android_wlr_log);

    const char *xw_path = getenv("WLR_XWAYLAND");
    LOGI("WLR_XWAYLAND=%s", xw_path ? xw_path : "(null)");

    session_environment_init();
    rcxml_read(NULL); /* default config */

    /* Force non-lazy XWayland startup on Android. Without this, XWayland
     * only starts when an X11 client connects — but the PRoot terminal
     * waits for the X socket first (chicken-and-egg). */
    {
        extern struct rcxml rc;
        rc.xwayland_persistence = true;
        LOGI("Forced xwayland_persistence=true (non-lazy)");
    }

    /* Try GPU path: GLES2 renderer + AHardwareBuffer allocator */
    android_renderer = create_android_gles2_renderer();
    if (android_renderer) {
        android_allocator = wlr_ahb_allocator_create();
        if (android_allocator) {
            LOGI("GPU path: GLES2 renderer + AHardwareBuffer allocator");
        } else {
            LOGE("AHB allocator failed, falling back to pixman");
            wlr_renderer_destroy(android_renderer);
            android_renderer = NULL;
        }
    }
    /* Fallback: pixman + SHM (CPU path) */
    if (!android_renderer) {
        LOGI("Using pixman (software) renderer");
        android_allocator = wlr_shm_allocator_create();
    }

    server_init();

    /* Force-load xkb keymap BEFORE server_start so clients get it on connect */
    {
        const char *xkb_root = getenv("XKB_CONFIG_ROOT");
        LOGI("XKB_CONFIG_ROOT=%s", xkb_root ? xkb_root : "(null)");
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_SECURE_GETENV |
                                                    XKB_CONTEXT_NO_DEFAULT_INCLUDES);
        if (xkb_root) {
            xkb_context_include_path_append(ctx, xkb_root);
        }
        struct xkb_rule_names names = { .rules = "evdev", .model = "pc105",
                                         .layout = "us" };
        struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names,
                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap) {
            LOGI("XKB keymap loaded successfully");
            struct wlr_keyboard *kb = &server.seat.keyboard_group->keyboard;
            wlr_keyboard_set_keymap(kb, keymap);
            wlr_seat_set_keyboard(server.seat.wlr_seat, kb);
            /* Headless backend has no physical devices, so keyboard capability
             * is never advertised. Set it explicitly so clients bind wl_keyboard. */
            uint32_t caps = server.seat.wlr_seat->capabilities;
            wlr_seat_set_capabilities(server.seat.wlr_seat,
                caps | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
            LOGI("Keyboard set on seat: kb=%p caps=0x%x", kb,
                 server.seat.wlr_seat->capabilities);
            xkb_keymap_unref(keymap);
        } else {
            LOGE("Failed to load XKB keymap from %s", xkb_root ? xkb_root : "(null)");
        }
        xkb_context_unref(ctx);
    }

    server_start();

    /* Make socket accessible to external clients (Termux, chroot, etc.)
     * External clients connect with:
     *   export XDG_RUNTIME_DIR=<dir containing wayland-0>
     *   export WAYLAND_DISPLAY=wayland-0 */
    chmod(socket_path_buf, 0666);
    /* Also chmod the lock file */
    {
        char lp[272];
        snprintf(lp, sizeof(lp), "%s.lock", socket_path_buf);
        chmod(lp, 0666);
    }
    LOGI("Wayland socket: %s (chmod 0666 for external access)", socket_path_buf);

    /* Initialize theme with built-in defaults (no external files needed).
     * Pango/Cairo resolve fonts via FONTCONFIG_FILE → /system/fonts. */
    memset(&g_theme, 0, sizeof(g_theme));
    theme_init(&g_theme, rc.theme_name);
    rc.theme = &g_theme;
    LOGI("Theme initialized: titlebar_height=%d", rc.theme->titlebar_height);
    menu_init();

    /* Hook output commit to blit frames to ANativeWindow */
    {
        struct output *lab_output;
        wl_list_for_each(lab_output, &server.outputs, link) {
            output_commit_listener.notify = on_output_commit;
            wl_signal_add(&lab_output->wlr_output->events.commit,
                          &output_commit_listener);
            LOGI("Output commit listener registered on %s",
                 lab_output->wlr_output->name);
            break; /* first output only */
        }
    }

    /* Periodic frame timer — forces output commit every 16ms (~60fps)
     * so new frames are blitted even when no client damage occurs */
    g_frame_timer = wl_event_loop_add_timer(
        server.wl_event_loop, frame_timer_cb, NULL);
    wl_event_source_timer_update(g_frame_timer, 100); /* first tick in 100ms */

    compositor_running = 1;
    LOGI("Compositor event loop starting");

    wl_display_run(server.wl_display);

    LOGI("Compositor shutting down");
    session_shutdown();
    rcxml_finish();
    font_finish();
    server_finish();
    compositor_running = 0;

    return NULL;
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeStart(
    JNIEnv *env, jclass cls,
    jstring jXdgRuntimeDir, jstring jXkbConfigRoot, jstring jFontconfigFile)
{
    if (compositor_running) {
        LOGI("Stopping previous compositor instance");
        wl_display_terminate(server.wl_display);
        pthread_join(compositor_thread, NULL);
        compositor_running = 0;
    }

    const char *xdgDir = (*env)->GetStringUTFChars(env, jXdgRuntimeDir, NULL);
    const char *xkbRoot = (*env)->GetStringUTFChars(env, jXkbConfigRoot, NULL);
    const char *fontConf = (*env)->GetStringUTFChars(env, jFontconfigFile, NULL);

    setenv("XDG_RUNTIME_DIR", xdgDir, 1);
    setenv("XKB_CONFIG_ROOT", xkbRoot, 1);
    setenv("FONTCONFIG_FILE", fontConf, 1);
    /* TMPDIR: Android has no /tmp. wlroots XWayland sockets use TMPDIR
     * to find .X11-unix/. Point to the parent of XDG dir (the app cache)
     * which PRoot also maps as /tmp inside the guest. */
    {
        char tmpdir[256];
        strncpy(tmpdir, xdgDir, sizeof(tmpdir) - 1);
        tmpdir[sizeof(tmpdir) - 1] = '\0';
        char *slash = strrchr(tmpdir, '/');
        if (slash) *slash = '\0'; /* cache/wayland-xdg → cache */
        setenv("TMPDIR", tmpdir, 1);
        LOGI("TMPDIR=%s", tmpdir);
        /* Create .X11-unix dir for XWayland sockets */
        char x11dir[280];
        snprintf(x11dir, sizeof(x11dir), "%s/.X11-unix", tmpdir);
        mkdir(x11dir, 0755);
    }
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
    setenv("LIBSEAT_BACKEND", "noop", 1);
    setenv("HOME", xdgDir, 1);
    setenv("XDG_DATA_HOME", xdgDir, 1);
    setenv("XDG_DATA_DIRS", xdgDir, 1);
    setenv("XDG_CONFIG_HOME", xdgDir, 1);
    setenv("XDG_CONFIG_DIRS", xdgDir, 1);

    /* Use app's XDG dir for the socket, but make it world-accessible so
     * external clients (Termux, chroot) can connect.
     * Path: /data/user/0/sh.haven.app/cache/wayland-xdg/wayland-0 */
    setenv("XDG_RUNTIME_DIR", xdgDir, 1);
    /* Make the directory and ancestors traversable by other apps */
    chmod(xdgDir, 0755);

    snprintf(socket_path_buf, sizeof(socket_path_buf), "%s/wayland-0", xdgDir);

    /* Clean stale sockets */
    char lock_path[272];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path_buf);
    unlink(socket_path_buf);
    unlink(lock_path);

    LOGI("Starting compositor: XDG_RUNTIME_DIR=%s", xdgDir);

    (*env)->ReleaseStringUTFChars(env, jXdgRuntimeDir, xdgDir);
    (*env)->ReleaseStringUTFChars(env, jXkbConfigRoot, xkbRoot);
    (*env)->ReleaseStringUTFChars(env, jFontconfigFile, fontConf);

    pthread_create(&compositor_thread, NULL, compositor_main, NULL);
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (!compositor_running) return;
    LOGI("Stopping compositor");
    wl_display_terminate(server.wl_display);
    pthread_join(compositor_thread, NULL);
    LOGI("Compositor stopped");
}

JNIEXPORT jstring JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeGetSocketPath(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, socket_path_buf);
}

JNIEXPORT jboolean JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeIsRunning(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return compositor_running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeSendTouch(
    JNIEnv *env, jclass cls, jint action, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    /* Android MotionEvent actions: 0=DOWN, 1=UP, 2=MOVE */
    if (action == 2) { /* MOVE */
        queue_input((struct input_event){
            .type = INPUT_MOTION, .x = x, .y = y });
    } else if (action == 0) { /* DOWN */
        queue_input((struct input_event){
            .type = INPUT_MOTION, .x = x, .y = y });
        queue_input((struct input_event){
            .type = INPUT_BUTTON, .code = 0x110, .pressed = 1 }); /* BTN_LEFT */
    } else if (action == 1) { /* UP */
        queue_input((struct input_event){
            .type = INPUT_BUTTON, .code = 0x110, .pressed = 0 });
    }
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeSendKey(
    JNIEnv *env, jclass cls, jint linuxKeyCode, jint pressed)
{
    (void)env; (void)cls;
    queue_input((struct input_event){
        .type = INPUT_KEY, .code = (uint32_t)linuxKeyCode, .pressed = pressed });
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeSendScroll(
    JNIEnv *env, jclass cls, jint axis, jfloat value)
{
    (void)env; (void)cls;
    queue_input((struct input_event){
        .type = INPUT_SCROLL, .x = (double)value, .code = (uint32_t)axis });
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeResize(
    JNIEnv *env, jclass cls, jint width, jint height)
{
    (void)env; (void)cls;
    g_pending_view_w = width / WAYLAND_SCALE;
    g_pending_view_h = height / WAYLAND_SCALE;
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeSetZoom(
    JNIEnv *env, jclass cls, jint zoomPermille, jboolean commit)
{
    (void)env; (void)cls;
    if (zoomPermille < 500) zoomPermille = 500;
    if (zoomPermille > 4000) zoomPermille = 4000;
    g_zoom_permille = zoomPermille;

    if (g_max_surface_w > 0) {
        int mw = (int)((long)g_max_surface_w * 1000 / zoomPermille);
        int mh = (int)((long)g_max_surface_h * 1000 / zoomPermille);
        if (mw < 100) mw = 100;
        if (mh < 100) mh = 100;

        /* During pinch: only change ANativeWindow buffer (smooth visual zoom).
         * On commit (pinch end): also change compositor mode (foot reflows). */
        pthread_mutex_lock(&g_window_lock);
        if (g_window) {
            ANativeWindow_setBuffersGeometry(g_window, mw, mh,
                AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
        }
        pthread_mutex_unlock(&g_window_lock);

        if (commit) {
            g_pending_resize_w = mw;
            g_pending_resize_h = mh;
        }
    }
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeSetViewport(
    JNIEnv *env, jclass cls, jint x, jint y)
{
    (void)env; (void)cls;
    g_viewport_x = x;
    g_viewport_y = y;
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeSetSurface(JNIEnv *env, jclass cls, jobject surface)
{
    (void)cls;
    /* Destroy old presenter before releasing window */
    if (g_presenter) {
        buffer_presenter_destroy(g_presenter);
        g_presenter = NULL;
    }

    pthread_mutex_lock(&g_window_lock);
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = NULL;
    }
    if (surface) {
        g_window = ANativeWindow_fromSurface(env, surface);
        if (g_window) {
            int sw = ANativeWindow_getWidth(g_window);
            int sh = ANativeWindow_getHeight(g_window);
            LOGI("Surface set: %dx%d", sw, sh);

            /* Try zero-copy presenter (API 29+) */
            g_presenter = buffer_presenter_create(g_window);

            if (!g_presenter) {
                /* Fallback: set pixel format for CPU blit path */
                ANativeWindow_setBuffersGeometry(g_window, 0, 0,
                    AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
            }

            sw = ANativeWindow_getWidth(g_window);
            sh = ANativeWindow_getHeight(g_window);
            g_max_surface_w = sw;
            g_max_surface_h = sh;
            g_pending_resize_w = sw;
            g_pending_resize_h = sh;
            LOGI("Surface set: %dx%d (presenter=%s)", sw, sh,
                 g_presenter ? "zero-copy" : "CPU blit");

            /* Re-blit cached frame immediately so the surface isn't black */
            if (!g_presenter && g_last_frame && g_last_frame_w > 0) {
                ANativeWindow_Buffer nbuf;
                if (ANativeWindow_lock(g_window, &nbuf, NULL) == 0) {
                    int w = nbuf.width < g_last_frame_w ? nbuf.width : g_last_frame_w;
                    int h = nbuf.height < g_last_frame_h ? nbuf.height : g_last_frame_h;
                    size_t dst_stride = (size_t)nbuf.stride * 4;
                    uint8_t *src = (uint8_t *)g_last_frame;
                    uint8_t *dst = (uint8_t *)nbuf.bits;
                    memset(dst, 0, dst_stride * nbuf.height);
                    for (int y = 0; y < h; y++) {
                        memcpy(dst + y * dst_stride, src + y * g_last_frame_stride, w * 4);
                    }
                    ANativeWindow_unlockAndPost(g_window);
                    LOGI("Re-blitted cached frame %dx%d", w, h);
                }
            }
        }
    } else {
        LOGI("Surface cleared");
        /* Reset so next surface set triggers a fresh output resize */
        g_max_surface_w = 0;
        g_max_surface_h = 0;
    }
    pthread_mutex_unlock(&g_window_lock);
}

/* ========== Benchmark launcher ========== */

static pid_t g_benchmark_pid = -1;

JNIEXPORT jboolean JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeLaunchBenchmark(
        JNIEnv *env, jclass cls, jstring jBinaryPath) {
    /* Toggle: if running, kill it and return false */
    if (g_benchmark_pid > 0 && kill(g_benchmark_pid, 0) == 0) {
        LOGI("Stopping benchmark pid %d", g_benchmark_pid);
        kill(g_benchmark_pid, SIGTERM);
        g_benchmark_pid = -1;
        return JNI_FALSE;
    }

    const char *path = (*env)->GetStringUTFChars(env, jBinaryPath, NULL);
    if (!path) return JNI_FALSE;
    LOGI("Launching benchmark: %s", path);

    /* Extract XDG_RUNTIME_DIR from socket path (remove /wayland-0 suffix) */
    char xdg_dir[256] = "/tmp";
    if (socket_path_buf[0]) {
        strncpy(xdg_dir, socket_path_buf, sizeof(xdg_dir) - 1);
        xdg_dir[sizeof(xdg_dir) - 1] = '\0';
        char *slash = strrchr(xdg_dir, '/');
        if (slash) *slash = '\0';
    }

    pid_t pid = fork();
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("XDG_RUNTIME_DIR", xdg_dir, 1);
        execl(path, path, NULL);
        LOGE("execl failed: %s", strerror(errno));
        _exit(1);
    } else if (pid > 0) {
        g_benchmark_pid = pid;
        LOGI("Benchmark forked as pid %d", pid);
    } else {
        LOGE("fork failed: %s", strerror(errno));
        (*env)->ReleaseStringUTFChars(env, jBinaryPath, path);
        return JNI_FALSE;
    }

    (*env)->ReleaseStringUTFChars(env, jBinaryPath, path);
    return JNI_TRUE;
}

/* ========== virgl_test_server launcher ========== */

static pid_t g_virgl_pid = -1;

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeStartVirglServer(
        JNIEnv *env, jclass cls, jstring jBinaryPath, jstring jSocketPath) {
    if (g_virgl_pid > 0) {
        LOGI("virgl_test_server already running (pid %d)", g_virgl_pid);
        return;
    }

    const char *path = (*env)->GetStringUTFChars(env, jBinaryPath, NULL);
    const char *sock = (*env)->GetStringUTFChars(env, jSocketPath, NULL);
    if (!path || !sock) {
        LOGE("nativeStartVirglServer: null args");
        if (path) (*env)->ReleaseStringUTFChars(env, jBinaryPath, path);
        if (sock) (*env)->ReleaseStringUTFChars(env, jSocketPath, sock);
        return;
    }
    LOGI("Starting virgl_test_server: %s socket=%s", path, sock);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: ensure EGL/GLES2 are discoverable by the dynamic linker.
         * The app process has them via the runtime linker namespace, but
         * execl replaces the process image and loses that namespace. */
        setenv("LD_LIBRARY_PATH", "/system/lib64", 1);
        execl(path, path, "--use-egl-surfaceless", "--use-gles",
              "--socket-path", sock, "--no-fork", NULL);
        LOGE("execl virgl_test_server failed: %s", strerror(errno));
        _exit(1);
    } else if (pid > 0) {
        g_virgl_pid = pid;
        LOGI("virgl_test_server forked as pid %d", pid);
    } else {
        LOGE("fork failed: %s", strerror(errno));
    }

    (*env)->ReleaseStringUTFChars(env, jBinaryPath, path);
    (*env)->ReleaseStringUTFChars(env, jSocketPath, sock);
}

JNIEXPORT void JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeStopVirglServer(
        JNIEnv *env, jclass cls) {
    if (g_virgl_pid > 0) {
        LOGI("Stopping virgl_test_server pid %d", g_virgl_pid);
        kill(g_virgl_pid, SIGTERM);
        g_virgl_pid = -1;
    }
}

JNIEXPORT jboolean JNICALL
Java_sh_haven_core_wayland_WaylandBridge_nativeIsVirglRunning(
        JNIEnv *env, jclass cls) {
    if (g_virgl_pid <= 0) return JNI_FALSE;
    /* Check if process is still alive */
    if (kill(g_virgl_pid, 0) == 0) return JNI_TRUE;
    g_virgl_pid = -1;
    return JNI_FALSE;
}
