/*
 * AHardwareBuffer allocator for wlroots on Android.
 * Allocates GPU-accessible buffers via Android's AHardwareBuffer API.
 * The GLES2 renderer imports these via EGL_ANDROID_image_native_buffer.
 *
 * Based on Xtr126/labwc-android ahb_wlr_allocator.c.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <drm_fourcc.h>

/* native_handle_t is not in the NDK but AHardwareBuffer_getNativeHandle returns it.
 * Define the struct layout (stable ABI since Android 4.0). */
typedef struct native_handle {
	int version;   /* sizeof(native_handle_t) */
	int numFds;    /* number of file-descriptors at &data[0] */
	int numInts;   /* number of ints at &data[numFds] */
	int data[0];   /* numFds + numInts ints */
} native_handle_t;

/* Not in public NDK headers but exported by libandroid.so */
extern const native_handle_t *AHardwareBuffer_getNativeHandle(
	const AHardwareBuffer *buffer);
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
#include <wlr/util/log.h>
#include <wlr/render/drm_format_set.h>

#include "ahb_allocator.h"
#include "buffer_format_utils.h"

#define LOG_TAG "ahb-alloc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const struct wlr_buffer_impl buffer_impl;
static const struct wlr_allocator_interface allocator_impl;

struct wlr_ahb_buffer *try_get_ahb_buffer_from_buffer(
		struct wlr_buffer *wlr_buffer) {
	if (wlr_buffer->impl == &buffer_impl) {
		return wl_container_of(wlr_buffer, (struct wlr_ahb_buffer *)NULL, base);
	}
	return NULL;
}

static struct wlr_ahb_buffer *create_buffer(
		struct wlr_ahb_allocator *alloc,
		int width, int height,
		const struct wlr_drm_format *format) {
	uint32_t android_fmt = drm_to_android_format(format->format);
	if (android_fmt == 0) {
		/* If the requested DRM format can't be converted, try RGBX
		 * as a safe default for opaque compositing */
		LOGI("DRM format 0x%x not mappable to Android, using RGBX", format->format);
		android_fmt = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
	}

	struct wlr_ahb_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		return NULL;
	}

	AHardwareBuffer_Desc desc = {
		.width = width,
		.height = height,
		.layers = 1,
		.format = android_fmt,
		.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
		         AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
		         AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
		         AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
	};

	int ret = AHardwareBuffer_allocate(&desc, &buffer->ahb);
	if (ret != 0) {
		LOGE("AHardwareBuffer_allocate failed: %d (fmt=0x%x %dx%d)",
			ret, android_fmt, width, height);
		free(buffer);
		return NULL;
	}

	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	wl_list_insert(&alloc->buffers, &buffer->link);

	LOGI("AHardwareBuffer allocated: %dx%d fmt=0x%x", width, height, android_fmt);
	return buffer;
}

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_ahb_buffer *buffer =
		wl_container_of(wlr_buffer, buffer, base);
	if (buffer->ahb) {
		AHardwareBuffer_release(buffer->ahb);
	}
	wl_list_remove(&buffer->link);
	free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_ahb_buffer *buffer =
		wl_container_of(wlr_buffer, buffer, base);

	/* Extract DMA-BUF from AHardwareBuffer via native handle.
	 * The native_handle_t contains FDs (for DMA-BUF planes) and
	 * integer metadata (stride, format, etc). */
	const native_handle_t *handle = AHardwareBuffer_getNativeHandle(buffer->ahb);
	if (!handle || handle->numFds < 1) {
		LOGE("No FDs in native handle (numFds=%d)", handle ? handle->numFds : -1);
		return false;
	}

	AHardwareBuffer_Desc desc;
	AHardwareBuffer_describe(buffer->ahb, &desc);

	memset(attribs, 0, sizeof(*attribs));
	attribs->width = desc.width;
	attribs->height = desc.height;
	attribs->format = android_to_drm_format(desc.format);
	attribs->modifier = DRM_FORMAT_MOD_INVALID;
	attribs->n_planes = 1;
	attribs->fd[0] = handle->data[0]; /* first FD is the DMA-BUF */
	attribs->stride[0] = desc.stride * 4; /* stride in bytes */
	attribs->offset[0] = 0;

	return true;
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct wlr_ahb_buffer *buffer =
		wl_container_of(wlr_buffer, buffer, base);

	uint64_t usage = 0;
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_READ)
		usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		usage |= AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

	AHardwareBuffer_Desc desc;
	AHardwareBuffer_describe(buffer->ahb, &desc);

	void *ptr = NULL;
	int ret = AHardwareBuffer_lock(buffer->ahb, usage, -1, NULL, &ptr);
	if (ret != 0 || !ptr) {
		LOGE("AHardwareBuffer_lock failed: %d", ret);
		return false;
	}

	*data = ptr;
	*format = android_to_drm_format(desc.format);
	*stride = desc.stride * 4; /* stride in bytes */
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	struct wlr_ahb_buffer *buffer =
		wl_container_of(wlr_buffer, buffer, base);
	AHardwareBuffer_unlock(buffer->ahb, NULL);
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

/* Allocator interface */

static struct wlr_buffer *allocator_create_buffer(
		struct wlr_allocator *wlr_alloc,
		int width, int height,
		const struct wlr_drm_format *format) {
	struct wlr_ahb_allocator *alloc =
		wl_container_of(wlr_alloc, alloc, base);
	struct wlr_ahb_buffer *buffer = create_buffer(alloc, width, height, format);
	if (!buffer) {
		return NULL;
	}
	return &buffer->base;
}

static void allocator_destroy(struct wlr_allocator *wlr_alloc) {
	struct wlr_ahb_allocator *alloc =
		wl_container_of(wlr_alloc, alloc, base);
	struct wlr_ahb_buffer *buf, *tmp;
	wl_list_for_each_safe(buf, tmp, &alloc->buffers, link) {
		if (buf->ahb) {
			AHardwareBuffer_release(buf->ahb);
			buf->ahb = NULL;
		}
		wl_list_remove(&buf->link);
	}
	free(alloc);
}

static const struct wlr_allocator_interface allocator_impl = {
	.destroy = allocator_destroy,
	.create_buffer = allocator_create_buffer,
};

struct wlr_allocator *wlr_ahb_allocator_create(void) {
	struct wlr_ahb_allocator *alloc = calloc(1, sizeof(*alloc));
	if (!alloc) {
		return NULL;
	}
	/* Advertise DMABUF capability so the GLES2 renderer's buffer cap
	 * check passes (renderer requires DMABUF). The actual import uses
	 * EGL_ANDROID_image_native_buffer, not the DMA-BUF extension. */
	wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
	wl_list_init(&alloc->buffers);

	LOGI("AHardwareBuffer allocator created");
	return &alloc->base;
}
