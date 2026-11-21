/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibMediaCodecs/FFmpeg/MallocTuning.h>

#if defined(__GLIBC__)
#    include <malloc.h>
#endif

namespace Media::FFmpeg {

void set_fixed_mmap_threshold()
{
#if defined(__GLIBC__)
    // FFmpeg, system and bundled alike, allocates through glibc, including CPU-decoded video frame buffers. When one
    // of those buffers is freed, glibc normally raise its threshold past that size and serves the next ones from its
    // arenas, which keeps them resident after the decoder is gone. Set a fixed threshold to keep them mapped
    // separately, so they are unmapped when freed.
    if (mallopt(M_MMAP_THRESHOLD, 128 * KiB) == 0)
        warnln("Could not set the mmap threshold of malloc");
#endif
}

}
