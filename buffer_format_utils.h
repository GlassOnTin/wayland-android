/*
 * DRM FourCC ↔ Android AHardwareBuffer format conversion.
 */
#ifndef BUFFER_FORMAT_UTILS_H
#define BUFFER_FORMAT_UTILS_H

#include <stdint.h>
#include <stdbool.h>

uint32_t drm_to_android_format(uint32_t drm_format);
uint32_t android_to_drm_format(uint32_t android_format);
bool drm_format_can_convert_to_android(uint32_t drm_format);

#endif
