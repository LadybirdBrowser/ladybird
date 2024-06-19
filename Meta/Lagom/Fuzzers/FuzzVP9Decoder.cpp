/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Video/VP9/Decoder.h>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    Media::Video::VP9::Decoder vp9_decoder;
    (void)vp9_decoder.receive_sample(Duration::zero(), { data, size });
    return 0;
}
