/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitStream.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Types.h>

namespace Gfx {

// Can decode bitstreams encoded with VP8's and VP9's arithmetic boolean encoder.
class BooleanDecoder {
public:
    static ErrorOr<BooleanDecoder> initialize(ReadonlyBytes data);

    /* (9.2) */
    bool read_bool(u8 probability);
    u8 read_literal(u8 bits);

    ErrorOr<void> finish_decode();

private:
    using ValueType = size_t;
    static constexpr u8 reserve_bytes = sizeof(ValueType) - 1;
    static constexpr u8 reserve_bits = reserve_bytes * 8;

    BooleanDecoder(u8 const* data, u64 bytes_left)
        : m_data(data + 1)
        , m_bytes_left(bytes_left - 1)
        , m_range(255)
        , m_value(static_cast<ValueType>(*data) << reserve_bits)
        , m_value_bits_left(8)
    {
        fill_reservoir();
    }

    void fill_reservoir();

    u8 const* m_data;
    size_t m_bytes_left { 0 };
    bool m_overread { false };
    // This value will never exceed 255. If this is a u8, the compiler will generate a truncation in read_bool().
    u32 m_range { 0 };
    ValueType m_value { 0 };
    // Like above, this will never exceed reserve_bits, but will truncate if it is a u8.
    u32 m_value_bits_left { 0 };
};

}
