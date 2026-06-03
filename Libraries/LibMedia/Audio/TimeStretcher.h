/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>

namespace Audio {

class TimeStretcher {
public:
    virtual ~TimeStretcher() = default;

    virtual i64 preroll_frame_count() const = 0;
    virtual void flush(AK::Duration media_start_timestamp, i64 output_start_frame_index) = 0;

    virtual void set_rate(float) = 0;

    virtual void push_block(Media::AudioBlock const&) = 0;
    virtual Media::DecoderErrorOr<Media::AudioBlock> retrieve_block() = 0;
    virtual void signal_end_of_stream() = 0;
};

}
