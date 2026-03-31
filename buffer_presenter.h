/*
 * Zero-copy buffer presentation via ASurfaceControl (API 29+).
 * Submits AHardwareBuffers directly to SurfaceFlinger.
 */
#ifndef BUFFER_PRESENTER_H
#define BUFFER_PRESENTER_H

#include <android/native_window.h>
#include <android/hardware_buffer.h>

typedef struct buffer_presenter buffer_presenter_t;

/* Creates a presenter from an ANativeWindow. Returns NULL if API < 29. */
buffer_presenter_t *buffer_presenter_create(ANativeWindow *window);

/* Destroys the presenter and releases the ASurfaceControl. */
void buffer_presenter_destroy(buffer_presenter_t *p);

/* Submits an AHardwareBuffer for display. Zero-copy — the buffer goes
 * directly to SurfaceFlinger without CPU access. */
void buffer_presenter_send(buffer_presenter_t *p, AHardwareBuffer *ahb);

#endif
