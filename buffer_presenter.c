/*
 * Zero-copy buffer presentation via ASurfaceControl (API 29+).
 * Based on Xtr126/labwc-android buffer_presenter.cpp.
 */
/* Suppress NDK availability guards — we do runtime API level checks */
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ 1
#include "buffer_presenter.h"
#include <stdlib.h>
#include <android/log.h>
#include <android/api-level.h>
#include <android/surface_control.h>

#define LOG_TAG "buf-present"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct buffer_presenter {
    ASurfaceControl *surface_control;
};

buffer_presenter_t *buffer_presenter_create(ANativeWindow *window) {
    if (android_get_device_api_level() < 29) {
        LOGI("ASurfaceControl requires API 29+, current: %d",
             android_get_device_api_level());
        return NULL;
    }

    buffer_presenter_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->surface_control = ASurfaceControl_createFromWindow(window, "labwc");
    if (!p->surface_control) {
        LOGE("ASurfaceControl_createFromWindow failed");
        free(p);
        return NULL;
    }

    LOGI("ASurfaceControl created (zero-copy presentation)");
    return p;
}

void buffer_presenter_destroy(buffer_presenter_t *p) {
    if (!p) return;
    if (p->surface_control) {
        ASurfaceControl_release(p->surface_control);
    }
    free(p);
}

void buffer_presenter_send(buffer_presenter_t *p, AHardwareBuffer *ahb) {
    if (!p || !p->surface_control || !ahb) return;

    ASurfaceTransaction *tx = ASurfaceTransaction_create();
    if (!tx) {
        LOGE("ASurfaceTransaction_create failed");
        return;
    }

    /* Submit the AHardwareBuffer directly to SurfaceFlinger.
     * acquire_fence_fd = -1 means the buffer is ready immediately
     * (GPU rendering is already complete by the time we get here). */
    ASurfaceTransaction_setBuffer(tx, p->surface_control, ahb, -1);

    /* Set the buffer to be opaque */
    ASurfaceTransaction_setBufferTransparency(tx, p->surface_control,
        ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);

    ASurfaceTransaction_apply(tx);
    ASurfaceTransaction_delete(tx);
}
