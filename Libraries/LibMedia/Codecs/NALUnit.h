/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Types.h>

namespace Media::Codecs {

// H.264 and H.265 configuration records both prefix their stored parameter sets with a 16-bit length.
constexpr u8 CONFIGURATION_RECORD_NAL_UNIT_LENGTH_SIZE = 2;

// Walk length-prefixed NAL units without retaining their backing storage or allocating a collection.
class NALUnitIterator {
public:
    NALUnitIterator(ReadonlyBytes data, u8 length_size)
        : m_remaining_data(data)
        , m_length_size(length_size)
        , m_has_error(length_size < 1 || length_size > 4)
    {
    }

    Optional<ReadonlyBytes> next()
    {
        if (m_has_error || m_remaining_data.is_empty())
            return {};
        if (m_remaining_data.size() < m_length_size) {
            m_has_error = true;
            return {};
        }
        u32 length = 0;
        for (u8 index = 0; index < m_length_size; ++index)
            length = (length << 8) | m_remaining_data[index];
        m_remaining_data = m_remaining_data.slice(m_length_size);
        if (length == 0 || length > m_remaining_data.size()) {
            m_has_error = true;
            return {};
        }
        auto nal_unit = m_remaining_data.trim(length);
        m_remaining_data = m_remaining_data.slice(length);
        return nal_unit;
    }

    bool has_error() const { return m_has_error; }

    // Empty once a walk has failed, so that a caller cannot resume from an offset the walk never reached.
    ReadonlyBytes remaining_data() const { return m_has_error ? ReadonlyBytes {} : m_remaining_data; }

private:
    ReadonlyBytes m_remaining_data;
    u8 m_length_size;
    bool m_has_error;
};

}
