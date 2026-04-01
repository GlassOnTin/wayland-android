/*
 * XWayland wrapper for Android.
 * wlroots execs this binary as the XWayland server.
 * It runs the real Xwayland through PRoot so it has access to X11 libraries.
 *
 * Usage: Set WLR_XWAYLAND to this binary's path.
 * Environment: HAVEN_PROOT_BIN, HAVEN_PROOT_LOADER, HAVEN_PROOT_ROOTFS,
 *              HAVEN_CACHE_DIR, HAVEN_XDG_DIR must be set by the launcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <android/log.h>

#define TAG "xwayland-wrap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

int main(int argc, char *argv[]) {
    const char *proot = getenv("HAVEN_PROOT_BIN");
    const char *loader = getenv("HAVEN_PROOT_LOADER");
    const char *rootfs = getenv("HAVEN_PROOT_ROOTFS");
    const char *cache = getenv("HAVEN_CACHE_DIR");
    const char *xdg = getenv("HAVEN_XDG_DIR");

    if (!proot || !loader || !rootfs || !cache || !xdg) {
        LOGE("Missing env vars: proot=%s loader=%s rootfs=%s cache=%s xdg=%s",
             proot ?: "null", loader ?: "null", rootfs ?: "null",
             cache ?: "null", xdg ?: "null");
        return 1;
    }

    LOGI("XWayland wrapper: proot=%s rootfs=%s", proot, rootfs);

    /* Set PRoot env */
    setenv("PROOT_TMP_DIR", cache, 1);
    setenv("PROOT_LOADER", loader, 1);

    /* Build PRoot + Xwayland command line:
     * proot -0 --link2symlink -r ROOTFS -b /dev -b /proc -b /sys
     *   -b CACHE:/tmp -b XDG:/tmp/xdg-runtime
     *   /usr/bin/Xwayland [original args from wlroots] */
    int nargs = 16 + argc; /* proot args + Xwayland path + -ac + original args */
    char **args = calloc(nargs + 1, sizeof(char *));
    int i = 0;

    char bind_cache[512], bind_xdg[512];
    snprintf(bind_cache, sizeof(bind_cache), "%s:/tmp", cache);
    snprintf(bind_xdg, sizeof(bind_xdg), "%s:/tmp/xdg-runtime", xdg);

    args[i++] = (char *)proot;
    args[i++] = "-0";
    args[i++] = "--link2symlink";
    args[i++] = "-r";
    args[i++] = (char *)rootfs;
    args[i++] = "-b";
    args[i++] = "/dev";
    args[i++] = "-b";
    args[i++] = "/proc";
    args[i++] = "-b";
    args[i++] = "/sys";
    args[i++] = "-b";
    args[i++] = bind_cache;
    args[i++] = "-b";
    args[i++] = bind_xdg;
    args[i++] = "/usr/bin/Xwayland";
    args[i++] = "-ac"; /* Disable access control (no xauth needed) */

    /* Append original args (display number, -rootless, etc.) */
    for (int j = 1; j < argc; j++) {
        args[i++] = argv[j];
    }
    args[i] = NULL;

    LOGI("Exec: %s %s ... %s (%d args)", args[0], args[1], args[15], i);
    execvp(proot, args);
    LOGE("execvp failed: %s", strerror(errno));
    return 1;
}
