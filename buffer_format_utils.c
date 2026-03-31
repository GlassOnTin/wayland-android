/*
 * DRM FourCC ↔ Android AHardwareBuffer format conversion.
 * Based on Xtr126/labwc-android buffer_utils.cpp.
 */
#include "buffer_format_utils.h"
#include <android/hardware_buffer.h>
#include <drm_fourcc.h>

uint32_t drm_to_android_format(uint32_t drm_format) {
	switch (drm_format) {
	case DRM_FORMAT_ABGR8888:
		return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
		return AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
	case DRM_FORMAT_BGR888:
		return AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
	case DRM_FORMAT_RGB565:
		return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
	case DRM_FORMAT_ABGR16161616F:
		return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
	case DRM_FORMAT_ABGR2101010:
		return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
	/* ARGB/XRGB: Android uses ABGR byte order natively.
	 * Map these to the closest Android format (colors will be correct
	 * because the GPU handles the swizzle). */
	case DRM_FORMAT_ARGB8888:
		return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_XRGB8888:
		return AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
	default:
		return 0;
	}
}

uint32_t android_to_drm_format(uint32_t android_format) {
	switch (android_format) {
	case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
		return DRM_FORMAT_ABGR8888;
	case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
		return DRM_FORMAT_XBGR8888;
	case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
		return DRM_FORMAT_BGR888;
	case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
		return DRM_FORMAT_RGB565;
	case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
		return DRM_FORMAT_ABGR16161616F;
	case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
		return DRM_FORMAT_ABGR2101010;
	default:
		return 0;
	}
}

bool drm_format_can_convert_to_android(uint32_t drm_format) {
	return drm_to_android_format(drm_format) != 0;
}
