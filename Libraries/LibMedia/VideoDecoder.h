/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#include "DecoderError.h"

namespace Media {

enum class DecodeIntent : u8 {
    Reference,
    Output,
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() { }

    virtual void set_storage_freed_callback(Function<void()>) = 0;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) = 0;
    virtual void signal_end_of_stream() = 0;

    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp) = 0;

    virtual void flush() = 0;
};

}
