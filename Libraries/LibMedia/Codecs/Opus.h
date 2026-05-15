/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Forward.h>

namespace Media::Codecs {

class Opus {
public:
    static DecoderErrorOr<AK::Duration> parse_frame_duration(MediaStreamCursor&, size_t frame_size);
    static DecoderErrorOr<u32> parse_frame_duration_in_samples(MediaStreamCursor&, size_t frame_size);
};

}
