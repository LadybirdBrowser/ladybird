/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>

#include "DecoderError.h"

namespace Media {

class VideoDecoder {
public:
    virtual ~VideoDecoder() {};

    virtual DecoderErrorOr<void> receive_sample(Duration timestamp, ReadonlyBytes sample) = 0;
    DecoderErrorOr<void> receive_sample(Duration timestamp, ByteBuffer const& sample) { return receive_sample(timestamp, sample.span()); }
    virtual DecoderErrorOr<NonnullOwnPtr<VideoFrame>> get_decoded_frame() = 0;

    virtual void flush() = 0;
};

}
