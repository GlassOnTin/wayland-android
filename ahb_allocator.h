/*
 * AHardwareBuffer allocator for wlroots on Android.
 * Allocates GPU-accessible buffers that the GLES2 renderer can import
 * via EGL_ANDROID_image_native_buffer.
 */
#ifndef AHB_ALLOCATOR_H
#define AHB_ALLOCATOR_H

#include <android/hardware_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>

struct wlr_ahb_buffer {
	struct wlr_buffer base;
	AHardwareBuffer *ahb;
	struct wl_list link; /* wlr_ahb_allocator.buffers */
};

struct wlr_ahb_allocator {
	struct wlr_allocator base;
	struct wl_list buffers; /* wlr_ahb_buffer.link */
};

/**
 * Creates a new allocator for AHardwareBuffers.
 */
struct wlr_allocator *wlr_ahb_allocator_create(void);

/**
 * Returns the AHB buffer if wlr_buffer was created by this allocator,
 * NULL otherwise.
 */
struct wlr_ahb_buffer *try_get_ahb_buffer_from_buffer(
	struct wlr_buffer *wlr_buffer);

#endif /* AHB_ALLOCATOR_H */
