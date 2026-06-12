/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Subsampling.h>

#include "DecoderError.h"

namespace Media {

struct VideoFrameMetadata {
    AK::Duration timestamp;
    AK::Duration duration;
    Gfx::IntSize size;
    u8 bit_depth { 0 };
    Subsampling subsampling;
    CodingIndependentCodePoints cicp;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() { }

    virtual DecoderErrorOr<void> receive_coded_data(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data, Optional<AK::Duration> decode_timestamp = {}) = 0;
    DecoderErrorOr<void> receive_coded_data(AK::Duration timestamp, AK::Duration duration, ByteBuffer const& coded_data, Optional<AK::Duration> decode_timestamp = {}) { return receive_coded_data(timestamp, duration, coded_data.span(), decode_timestamp); }
    virtual void signal_end_of_stream() = 0;

    virtual DecoderErrorOr<VideoFrameMetadata> peek_next_output(CodingIndependentCodePoints const& container_cicp) = 0;
    virtual DecoderErrorOr<void> take_next_output_into(Gfx::YUVData&) = 0;

    virtual void flush() = 0;
};

}
