/*
 * Haven GPU Benchmark Client
 *
 * Native GLES2 Wayland client that renders a rotating 3D cube and submits
 * frames to the compositor. Uses wl_shm with GPU readback for maximum
 * compatibility (works even without EGL_EXT_image_dma_buf_import).
 *
 * Build: cross-compile with NDK as PIE executable (packaged as .so for APK extraction).
 * Run:   WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/path ./libbenchmark_gles.so
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <errno.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <android/log.h>
#include <android/hardware_buffer.h>

#define TAG "haven-bench"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Default size — overridden by compositor's toplevel configure */
static int win_width  = 640;
static int win_height = 480;
#define BPP    4

/* --- Wayland state --- */
static struct wl_display    *display;
static struct wl_registry   *registry;
static struct wl_compositor *compositor;
static struct wl_shm        *shm;
static struct xdg_wm_base   *wm_base;

static struct wl_surface    *surface;
static struct xdg_surface   *xdg_surf;
static struct xdg_toplevel  *toplevel;

static int configured = 0;
static int running = 1;

/* --- SHM buffer pool --- */
static struct wl_shm_pool *pool;
static struct wl_buffer    *buffer;
static void                *shm_data;
static int                  shm_fd;

/* --- EGL/GLES2 offscreen --- */
static EGLDisplay egl_dpy;
static EGLContext egl_ctx;
static GLuint     fbo, color_rb;

/* --- Timing --- */
static int frame_count = 0;
static struct timespec start_time;
static float rotation = 0.0f;

/* ========== Shaders ========== */

static const char *vert_src =
    "attribute vec4 aPos;\n"
    "attribute vec3 aNormal;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat3 uNormal;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vPos;\n"
    "void main() {\n"
    "    gl_Position = uMVP * aPos;\n"
    "    vNormal = uNormal * aNormal;\n"
    "    vPos = (uMVP * aPos).xyz;\n"
    "}\n";

static const char *frag_src =
    "precision mediump float;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vPos;\n"
    "void main() {\n"
    "    vec3 lightDir = normalize(vec3(0.5, 0.7, 1.0));\n"
    "    vec3 n = normalize(vNormal);\n"
    "    float diff = max(dot(n, lightDir), 0.0);\n"
    "    float amb = 0.25;\n"
    "    /* Derive face color from normal direction for distinct faces */\n"
    "    vec3 absN = abs(n);\n"
    "    vec3 baseColor;\n"
    "    if (absN.z > 0.5) baseColor = n.z > 0.0 ? vec3(0.2, 0.6, 1.0) : vec3(1.0, 0.4, 0.2);\n"
    "    else if (absN.y > 0.5) baseColor = n.y > 0.0 ? vec3(0.2, 0.9, 0.3) : vec3(0.9, 0.2, 0.5);\n"
    "    else baseColor = n.x > 0.0 ? vec3(1.0, 0.8, 0.1) : vec3(0.6, 0.3, 0.9);\n"
    "    vec3 color = baseColor * (amb + diff * 0.75);\n"
    "    gl_FragColor = vec4(color, 1.0);\n"
    "}\n";

static GLuint program;
static GLint loc_pos, loc_normal, loc_mvp, loc_normalmat;

/* ========== Math helpers ========== */

typedef float mat4[16];
typedef float mat3[9];

static void mat4_identity(mat4 m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Column-major multiply: out = a * b (OpenGL convention) */
static void mat4_multiply(mat4 out, const mat4 a, const mat4 b) {
    mat4 tmp;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            tmp[col * 4 + row] = 0;
            for (int k = 0; k < 4; k++)
                tmp[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
        }
    memcpy(out, tmp, sizeof(mat4));
}

/* All matrices are column-major (OpenGL convention):
 * m[col*4 + row], so m[12]=tx, m[13]=ty, m[14]=tz */

static void mat4_perspective(mat4 m, float fovy, float aspect, float near, float far) {
    memset(m, 0, sizeof(mat4));
    float f = 1.0f / tanf(fovy * 0.5f);
    m[0]  = f / aspect;         /* col 0, row 0 */
    m[5]  = f;                  /* col 1, row 1 */
    m[10] = (far + near) / (near - far);  /* col 2, row 2 */
    m[11] = -1.0f;              /* col 2, row 3 */
    m[14] = (2.0f * far * near) / (near - far); /* col 3, row 2 */
}

static void mat4_rotate_y(mat4 m, float angle) {
    mat4_identity(m);
    float c = cosf(angle), s = sinf(angle);
    m[0]  =  c;  /* col 0, row 0 */
    m[2]  = -s;  /* col 0, row 2 */
    m[8]  =  s;  /* col 2, row 0 */
    m[10] =  c;  /* col 2, row 2 */
}

static void mat4_rotate_x(mat4 m, float angle) {
    mat4_identity(m);
    float c = cosf(angle), s = sinf(angle);
    m[5]  =  c;  /* col 1, row 1 */
    m[6]  =  s;  /* col 1, row 2 */
    m[9]  = -s;  /* col 2, row 1 */
    m[10] =  c;  /* col 2, row 2 */
}

static void mat4_translate(mat4 m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;  /* col 3, rows 0-2 */
}

static void mat4_to_mat3_normal(mat3 out, const mat4 m) {
    /* Upper-left 3x3 in column-major */
    out[0] = m[0]; out[1] = m[1]; out[2] = m[2];   /* col 0 */
    out[3] = m[4]; out[4] = m[5]; out[5] = m[6];   /* col 1 */
    out[6] = m[8]; out[7] = m[9]; out[8] = m[10];  /* col 2 */
}

/* ========== Cube geometry ========== */

/* 36 vertices (6 faces * 2 triangles * 3 verts), each: x,y,z, nx,ny,nz */
static const float cube_verts[] = {
    /* Front face */
    -1,-1, 1,  0, 0, 1,   1,-1, 1,  0, 0, 1,   1, 1, 1,  0, 0, 1,
    -1,-1, 1,  0, 0, 1,   1, 1, 1,  0, 0, 1,  -1, 1, 1,  0, 0, 1,
    /* Back face */
     1,-1,-1,  0, 0,-1,  -1,-1,-1,  0, 0,-1,  -1, 1,-1,  0, 0,-1,
     1,-1,-1,  0, 0,-1,  -1, 1,-1,  0, 0,-1,   1, 1,-1,  0, 0,-1,
    /* Top face */
    -1, 1, 1,  0, 1, 0,   1, 1, 1,  0, 1, 0,   1, 1,-1,  0, 1, 0,
    -1, 1, 1,  0, 1, 0,   1, 1,-1,  0, 1, 0,  -1, 1,-1,  0, 1, 0,
    /* Bottom face */
    -1,-1,-1,  0,-1, 0,   1,-1,-1,  0,-1, 0,   1,-1, 1,  0,-1, 0,
    -1,-1,-1,  0,-1, 0,   1,-1, 1,  0,-1, 0,  -1,-1, 1,  0,-1, 0,
    /* Right face */
     1,-1, 1,  1, 0, 0,   1,-1,-1,  1, 0, 0,   1, 1,-1,  1, 0, 0,
     1,-1, 1,  1, 0, 0,   1, 1,-1,  1, 0, 0,   1, 1, 1,  1, 0, 0,
    /* Left face */
    -1,-1,-1, -1, 0, 0,  -1,-1, 1, -1, 0, 0,  -1, 1, 1, -1, 0, 0,
    -1,-1,-1, -1, 0, 0,  -1, 1, 1, -1, 0, 0,  -1, 1,-1, -1, 0, 0,
};

/* ========== Shader compile ========== */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int init_shaders(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vs || !fs) return -1;

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("Program link error: %s", log);
        return -1;
    }

    loc_pos = glGetAttribLocation(program, "aPos");
    loc_normal = glGetAttribLocation(program, "aNormal");
    loc_mvp = glGetUniformLocation(program, "uMVP");
    loc_normalmat = glGetUniformLocation(program, "uNormal");

    glDeleteShader(vs);
    glDeleteShader(fs);
    return 0;
}

/* ========== EGL offscreen setup ========== */

static int init_egl(void) {
    egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_dpy == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_dpy, &major, &minor)) {
        LOGE("eglInitialize failed");
        return -1;
    }
    LOGI("EGL %d.%d", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, 0, /* surfaceless */
        EGL_NONE,
    };
    EGLConfig config;
    EGLint ncfg;
    if (!eglChooseConfig(egl_dpy, cfg_attr, &config, 1, &ncfg) || ncfg == 0) {
        LOGE("eglChooseConfig failed");
        return -1;
    }

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attr);
    if (egl_ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return -1;
    }

    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        LOGE("eglMakeCurrent failed");
        return -1;
    }

    LOGI("GL_RENDERER: %s", glGetString(GL_RENDERER));

    /* Create FBO with color + depth renderbuffers for offscreen rendering */
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenRenderbuffers(1, &color_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, color_rb);
    /* Use GL_RGBA8_OES if available, fall back to GL_RGBA4 */
    const char *gl_exts = (const char *)glGetString(GL_EXTENSIONS);
    GLenum color_fmt = GL_RGBA4;
    if (gl_exts && strstr(gl_exts, "GL_OES_rgb8_rgba8")) {
        color_fmt = GL_RGBA8_OES;
        LOGI("Using GL_RGBA8_OES renderbuffer");
    } else {
        LOGI("GL_OES_rgb8_rgba8 not available, using GL_RGBA4");
    }
    glRenderbufferStorage(GL_RENDERBUFFER, color_fmt, win_width, win_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, color_rb);

    GLuint depth_rb;
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, win_width, win_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_rb);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO incomplete: 0x%x", status);
        return -1;
    }

    LOGI("Offscreen FBO %dx%d ready (color=0x%x + D16), status=0x%x",
         win_width, win_height, color_fmt, status);

    /* Verify glReadPixels works with this FBO */
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    uint8_t test[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, test);
    LOGI("FBO readback test: R=%d G=%d B=%d A=%d (expect 255,0,0,255)",
         test[0], test[1], test[2], test[3]);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOGE("GL error after FBO setup: 0x%x", err);
    }

    return 0;
}

/* ========== SHM buffer setup ========== */

static int create_shm_buffer(void) {
    int stride = win_width * BPP;
    int size = stride * win_height;

    /* Create anonymous shared memory (syscall because NDK API 28 lacks the wrapper) */
    shm_fd = syscall(SYS_memfd_create, "haven-bench", MFD_CLOEXEC);
    if (shm_fd < 0) {
        LOGE("memfd_create failed: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(shm_fd, size) < 0) {
        LOGE("ftruncate failed: %s", strerror(errno));
        close(shm_fd);
        return -1;
    }

    shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_data == MAP_FAILED) {
        LOGE("mmap failed: %s", strerror(errno));
        close(shm_fd);
        return -1;
    }

    pool = wl_shm_create_pool(shm, shm_fd, size);
    buffer = wl_shm_pool_create_buffer(pool, 0, win_width, win_height, stride,
                                        WL_SHM_FORMAT_ABGR8888);
    LOGI("SHM buffer created: %dx%d stride=%d", win_width, win_height, stride);
    return 0;
}

/* ========== Render a frame ========== */

static void render_frame(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, win_width, win_height);

    /* First 120 frames: draw colored quadrants to identify coordinate origin.
     * Top-left=Red, Top-right=Green, Bottom-left=Blue, Bottom-right=Yellow.
     * After that: rotating 3D cube. */
    if (frame_count < 120) {
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Use a simple pass-through shader: position only, flat color via uniform */
        glUseProgram(program);

        /* Draw 4 colored quads using scissor test */
        glEnable(GL_SCISSOR_TEST);

        /* Top-left = RED (GL coords: left=0, bottom=win_height/2) */
        glScissor(0, win_height/2, win_width/2, win_height/2);
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Top-right = GREEN (GL coords: left=win_width/2, bottom=win_height/2) */
        glScissor(win_width/2, win_height/2, win_width/2, win_height/2);
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Bottom-left = BLUE (GL coords: left=0, bottom=0) */
        glScissor(0, 0, win_width/2, win_height/2);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Bottom-right = YELLOW (GL coords: left=win_width/2, bottom=0) */
        glScissor(win_width/2, 0, win_width/2, win_height/2);
        glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glDisable(GL_SCISSOR_TEST);
    } else {
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(program);

        /* Build MVP matrix */
        mat4 proj, view, model, ry, rx, mv, mvp;
        mat4_perspective(proj, 45.0f * M_PI / 180.0f, (float)win_width / win_height, 0.1f, 100.0f);
        mat4_translate(view, 0, 0, -8.0f);
        mat4_rotate_y(ry, rotation);
        mat4_rotate_x(rx, rotation * 0.7f);
        mat4_multiply(model, ry, rx);
        mat4_multiply(mv, view, model);
        mat4_multiply(mvp, proj, mv);

        mat3 normalmat;
        mat4_to_mat3_normal(normalmat, mv);

        glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix3fv(loc_normalmat, 1, GL_FALSE, normalmat);

        /* Draw cube */
        glEnableVertexAttribArray(loc_pos);
        glEnableVertexAttribArray(loc_normal);
        glVertexAttribPointer(loc_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), cube_verts);
        glVertexAttribPointer(loc_normal, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), cube_verts + 3);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        glDisableVertexAttribArray(loc_pos);
        glDisableVertexAttribArray(loc_normal);
    }

    /* Log GL errors on first frame only */
    if (frame_count == 0) {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOGE("GL error after draw: 0x%x", err);
        } else {
            LOGI("Draw call OK");
        }
    }

    /* Readback to SHM buffer */
    glReadPixels(0, 0, win_width, win_height, GL_RGBA, GL_UNSIGNED_BYTE, shm_data);

    /* Note: GL readback is normally bottom-up, but testing showed the
     * compositor displays the buffer correctly without flipping. Skipping
     * the flip avoids a per-frame CPU copy. If the image appears upside
     * down on some devices, re-enable the flip. */

    /* Sample center pixel to verify rendering */
    if (frame_count == 0 || frame_count == 121) {
        uint8_t *px = (uint8_t *)shm_data;
        int cx = win_width / 2, cy = win_height / 2;
        int idx = (cy * win_width + cx) * BPP;
        LOGI("Frame %d center pixel: R=%d G=%d B=%d A=%d (bg=~13,13,31; cube=varied)",
             frame_count, px[idx], px[idx+1], px[idx+2], px[idx+3]);
        /* Also log uniform locations */
        if (frame_count == 121) {
            LOGI("Uniform locations: mvp=%d normalmat=%d pos=%d normal=%d",
                 loc_mvp, loc_normalmat, loc_pos, loc_normal);
        }
    }

    rotation += 0.02f;
    frame_count++;

    /* Log FPS every 600 frames (~10 seconds at 60fps) */
    if (frame_count % 600 == 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start_time.tv_sec) +
                         (now.tv_nsec - start_time.tv_nsec) / 1e9;
        LOGI("FPS: %.1f (%d frames in %.1fs)", frame_count / elapsed, frame_count, elapsed);
    }
}

/* ========== Wayland callbacks ========== */

static void xdg_surface_configure(void *data, struct xdg_surface *surf, uint32_t serial) {
    xdg_surface_ack_configure(surf, serial);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                    int32_t w, int32_t h, struct wl_array *states) {
    if (w > 0 && h > 0) {
        win_width = w;
        win_height = h;
        LOGI("Toplevel configure: %dx%d", w, h);
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl) {
    running = 0;
}

static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *tl,
                                           int32_t w, int32_t h) {}
static void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl,
                                          struct wl_array *caps) {}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

/* Frame callback — triggers next frame */
static struct wl_callback *frame_cb;
static void frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    wl_callback_destroy(cb);

    if (!running) return;

    render_frame();

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, win_width, win_height);

    frame_cb = wl_surface_frame(surface);
    wl_callback_add_listener(frame_cb, &frame_listener, NULL);

    wl_surface_commit(surface);
}

/* Registry */
static void registry_global(void *data, struct wl_registry *reg,
                             uint32_t name, const char *interface, uint32_t ver) {
    LOGI("Global: %s v%d", interface, ver);
    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 4);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* ========== Main ========== */

int main(int argc, char *argv[]) {
    LOGI("Haven GPU Benchmark starting...");

    /* Connect to Wayland compositor */
    display = wl_display_connect(NULL);
    if (!display) {
        LOGE("Failed to connect to Wayland display");
        return 1;
    }
    LOGI("Connected to Wayland display");

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        LOGE("Missing required globals: compositor=%p shm=%p wm_base=%p",
             compositor, shm, wm_base);
        return 1;
    }

    /* Create Wayland window first to get the compositor's preferred size */
    surface = wl_compositor_create_surface(compositor);
    xdg_surf = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surf, &xdg_surface_listener, NULL);

    toplevel = xdg_surface_get_toplevel(xdg_surf);
    xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Haven GPU Benchmark");
    xdg_toplevel_set_app_id(toplevel, "sh.haven.benchmark");

    /* Request maximized so the compositor sends its full output size */
    xdg_toplevel_set_maximized(toplevel);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    /* Wait for configure (sets win_width/win_height from compositor) */
    while (!configured) {
        wl_display_dispatch(display);
    }
    /* Dispatch once more to pick up the second configure with actual size */
    wl_display_roundtrip(display);

    LOGI("Window configured at %dx%d", win_width, win_height);

    /* Initialize EGL + GLES2 offscreen rendering at the configured size */
    if (init_egl() < 0) return 1;
    if (init_shaders() < 0) return 1;
    if (create_shm_buffer() < 0) return 1;

    LOGI("Starting render loop");
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Render first frame and start callback chain */
    render_frame();
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, win_width, win_height);

    frame_cb = wl_surface_frame(surface);
    wl_callback_add_listener(frame_cb, &frame_listener, NULL);
    wl_surface_commit(surface);

    /* Event loop */
    while (running && wl_display_dispatch(display) != -1) {
        /* Frame rendering happens in frame_done callback */
    }

    LOGI("Benchmark complete: %d frames rendered", frame_count);

    /* Cleanup */
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (xdg_surf) xdg_surface_destroy(xdg_surf);
    if (surface) wl_surface_destroy(surface);
    if (buffer) wl_buffer_destroy(buffer);
    if (pool) wl_shm_pool_destroy(pool);
    if (shm_data) munmap(shm_data, win_width * win_height * BPP);
    if (shm_fd >= 0) close(shm_fd);

    glDeleteRenderbuffers(1, &color_rb);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(program);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);

    wl_display_disconnect(display);
    return 0;
}
