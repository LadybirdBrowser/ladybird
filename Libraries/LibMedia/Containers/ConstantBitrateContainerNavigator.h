/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "ContainerNavigator.h"

namespace Media {

class ConstantBitrateContainerNavigator final : public ContainerNavigator {
public:
    ConstantBitrateContainerNavigator(size_t first_sample_position, u32 bytes_per_second, u32 block_align)
        : m_first_sample_position(first_sample_position)
        , m_bytes_per_second(bytes_per_second)
        , m_block_align(block_align)
    {
    }

    TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const override;
    virtual DecoderErrorOr<SeekResult> seek_to_timestamp(AK::Duration timestamp) const override;

private:
    size_t m_first_sample_position;
    size_t m_bytes_per_second;
    size_t m_block_align;
};

}
