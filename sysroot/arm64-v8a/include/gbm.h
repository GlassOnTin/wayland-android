/* Stub gbm.h for Android — GBM is not available on Android.
 * Provides type definitions so wlroots egl.c compiles, but the
 * GBM code paths are never reached (we use wlr_egl_create_with_context). */
#ifndef GBM_H
#define GBM_H

struct gbm_device;

static inline struct gbm_device *gbm_create_device(int fd) { (void)fd; return NULL; }
static inline void gbm_device_destroy(struct gbm_device *dev) { (void)dev; }
static inline int gbm_device_get_fd(struct gbm_device *dev) { (void)dev; return -1; }

#endif /* GBM_H */
