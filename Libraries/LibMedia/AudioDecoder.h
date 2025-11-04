/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>

namespace Media {

class AudioDecoder {
public:
    virtual ~AudioDecoder() { }

    virtual DecoderErrorOr<void> receive_coded_data(AK::Duration timestamp, ReadonlyBytes coded_data) = 0;
    virtual void signal_end_of_stream() = 0;
    // Writes all buffered audio samples to the provided block.
    virtual DecoderErrorOr<void> write_next_block(AudioBlock&) = 0;

    virtual void flush() = 0;
};

}
