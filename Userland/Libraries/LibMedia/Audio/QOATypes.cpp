/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "QOATypes.h"
#include <AK/Endian.h>
#include <AK/Stream.h>

namespace Audio::QOA {

ErrorOr<FrameHeader> FrameHeader::read_from_stream(Stream& stream)
{
    FrameHeader header;
    header.num_channels = TRY(stream.read_value<u8>());
    u8 sample_rate[3];
    // Enforce the order of the reads here, since the order of expression evaluations further down is implementation-defined.
    sample_rate[0] = TRY(stream.read_value<u8>());
    sample_rate[1] = TRY(stream.read_value<u8>());
    sample_rate[2] = TRY(stream.read_value<u8>());
    header.sample_rate = (sample_rate[0] << 16) | (sample_rate[1] << 8) | sample_rate[2];
    header.sample_count = TRY(stream.read_value<BigEndian<u16>>());
    header.frame_size = TRY(stream.read_value<BigEndian<u16>>());
    return header;
}

LMSState::LMSState(u64 history_packed, u64 weights_packed)
{
    for (size_t i = 0; i < lms_history; ++i) {
        // The casts ensure proper sign extension.
        history[i] = static_cast<i16>(history_packed >> 48);
        history_packed <<= 16;
        weights[i] = static_cast<i16>(weights_packed >> 48);
        weights_packed <<= 16;
    }
}

i32 LMSState::predict() const
{
    // The spec specifies that overflows are not allowed, but we do a safe thing anyways.
    Checked<i32> prediction = 0;
    for (size_t i = 0; i < lms_history; ++i)
        prediction.saturating_add(Checked<i32>::saturating_mul(history[i], weights[i]));
    return prediction.value() >> 13;
}

void LMSState::update(i32 sample, i32 residual)
{
    i32 delta = residual >> 4;
    for (size_t i = 0; i < lms_history; ++i)
        weights[i] += history[i] < 0 ? -delta : delta;

    for (size_t i = 0; i < lms_history - 1; ++i)
        history[i] = history[i + 1];
    history[lms_history - 1] = sample;
}

}
