/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Code that reaches into FFmpeg's structs only works with the libavcodec major version whose headers it was compiled
// against. Such code is compiled against the headers Ladybird is built with, and again against each other release's,
// to run with a system FFmpeg of that version. Those builds define LADYBIRD_SYSTEM_FFMPEG_NAMESPACE, and keep FFmpeg's
// declarations and their own in that namespace, since each sees different definitions of the same structs.
//
// Code that uses FFmpeg's declarations goes in FFMPEG_ABI_NAMESPACE.

#ifdef LADYBIRD_SYSTEM_FFMPEG_NAMESPACE

// FFmpeg's headers include these, which must not be declared again inside the namespace.
#    include <errno.h>
#    include <inttypes.h>
#    include <limits.h>
#    include <math.h>
#    include <stdarg.h>
#    include <stddef.h>
#    include <stdint.h>
#    include <stdio.h>
#    include <stdlib.h>
#    include <string.h>
#    ifdef USE_VULKAN_DMABUF_IMAGES
#        include <vulkan/vulkan.h>
#    endif

#    define FFMPEG_ABI_NAMESPACE Media::FFmpeg::LADYBIRD_SYSTEM_FFMPEG_NAMESPACE

// Every FFmpeg header that such code uses is included here, so that none is included outside the namespace.
namespace FFMPEG_ABI_NAMESPACE {

extern "C" {
#    include <libavcodec/avcodec.h>
#    include <libavutil/hwcontext.h>
#    include <libavutil/opt.h>
#    ifdef USE_VULKAN_DMABUF_IMAGES
#        include <libavutil/hwcontext_vulkan.h>
#    endif
}

}

#else

#    define FFMPEG_ABI_NAMESPACE Media::FFmpeg

extern "C" {
#    include <libavcodec/avcodec.h>
#    include <libavutil/hwcontext.h>
#    include <libavutil/opt.h>
#    include <libswresample/swresample.h>
}

#endif
