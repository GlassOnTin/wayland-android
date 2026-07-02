/*
 * libhaven_wayvnc_shim.c
 *
 * LD_PRELOAD shim that hides the ext-image-copy-capture-v1 protocol
 * from wayvnc so it falls back to zwlr_screencopy_manager_v1. wayvnc
 * 0.9.x prefers ext-image-copy-capture when both globals are bound, but
 * the wlroots-0.19 headless backend (used by Haven's nested-Wayland
 * compositors) reports zero buffer formats on the ext capture session,
 * so wayvnc bails with "No supported buffer formats were found". The
 * wlr-screencopy fallback path captures correctly.
 *
 * Hook target is wl_proxy_marshal_array_flags rather than
 * wl_registry_bind:
 *   - Modern wayland-scanner inlines wl_registry_bind into the calling
 *     client; there is no runtime symbol to intercept.
 *   - wayvnc's compiled binding therefore calls wl_proxy_marshal_flags
 *     (variadic) directly. Inside libwayland-client.so that varargs
 *     entry goes through PLT to wl_proxy_marshal_array_flags, so the
 *     LD_PRELOAD hook on the array-form catches every registry bind.
 *
 * Build:
 *   - Must target glibc (libc.so.6), NOT bionic. The NDK clang produces
 *     a .so with DT_NEEDED libdl.so / libc.so (Android filenames) which
 *     Arch / Alpine / Debian glibc cannot resolve.
 *   - aarch64: aarch64-linux-gnu-gcc
 *   - x86_64:  x86_64-linux-gnu-gcc
 *   - Use build.sh, which pins the glibc baseline (see below) and
 *     verifies the result. Building by hand on a modern host reintroduces
 *     the #246 GLIBC_2.34 dependency.
 *
 * Loaded by the proot launch script via:
 *   LD_PRELOAD=/usr/local/lib/haven/libhaven_wayvnc_shim.so
 *   exec wayvnc ...
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Pin dlsym to the per-arch glibc baseline so the shim loads on older
 * glibc. glibc merged libdl into libc at 2.34, versioning dlsym as
 * dlsym@GLIBC_2.34; building on a host with glibc >= 2.34 therefore
 * stamps that requirement into the .so. On a target whose preload-time
 * libc can't satisfy GLIBC_2.34, ld.so drops the LD_PRELOAD with a
 * misleading "cannot open shared object file" and wayvnc falls back to
 * the ext-image-copy-capture path that grey-screens on the wlroots
 * headless backend (#246, observed on Void). dlsym is the only symbol
 * the shim pulls in above the baseline, so pinning it keeps the .so at
 * NEEDED libc.so.6 only with no GLIBC_2.34.
 */
#if defined(__aarch64__)
__asm__(".symver dlsym, dlsym@GLIBC_2.17");
#elif defined(__x86_64__)
__asm__(".symver dlsym, dlsym@GLIBC_2.2.5");
#elif defined(__arm__)
/* 32-bit ARM (armhf): dlsym's baseline is the armhf port's first release. */
__asm__(".symver dlsym, dlsym@GLIBC_2.4");
#endif

struct wl_proxy;
struct wl_object;
struct wl_array;

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;
    int event_count;
    const void *events;
};

union wl_argument {
    int32_t i;
    uint32_t u;
    uint32_t f;
    const char *s;
    struct wl_object *o;
    uint32_t n;
    struct wl_array *a;
    int32_t h;
};

extern const char *wl_proxy_get_class(struct wl_proxy *proxy);

static const char *BLOCKED[] = {
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    NULL,
};

typedef struct wl_proxy *(*marshal_array_flags_fn)(
    struct wl_proxy *, uint32_t, const struct wl_interface *,
    uint32_t, uint32_t, union wl_argument *);

struct wl_proxy *wl_proxy_marshal_array_flags(
        struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *interface,
        uint32_t version, uint32_t flags, union wl_argument *args) {
    static marshal_array_flags_fn real = NULL;
    if (!real)
        real = (marshal_array_flags_fn)dlsym(
            RTLD_NEXT, "wl_proxy_marshal_array_flags");

    if (proxy && opcode == 0 && args) {
        const char *cls = wl_proxy_get_class(proxy);
        if (cls && strcmp(cls, "wl_registry") == 0) {
            const char *iface_name = args[1].s;
            if (iface_name) {
                for (const char **b = BLOCKED; *b; b++) {
                    if (strcmp(iface_name, *b) == 0) {
                        fprintf(stderr,
                                "[haven_wayvnc_shim] blocked %s\n",
                                iface_name);
                        return NULL;
                    }
                }
            }
        }
    }
    return real(proxy, opcode, interface, version, flags, args);
}

__attribute__((constructor))
static void haven_wayvnc_shim_init(void) {
    fprintf(stderr, "[haven_wayvnc_shim] loaded\n");
}
