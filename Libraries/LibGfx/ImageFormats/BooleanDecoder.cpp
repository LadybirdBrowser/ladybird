/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/Debug.h>
#include <AK/Endian.h>

#include "BooleanDecoder.h"

namespace Gfx {

// 9.2.1 Initialization process for Boolean decoder
ErrorOr<BooleanDecoder> BooleanDecoder::initialize(ReadonlyBytes data)
{
    if (data.size() == 0)
        return Error::from_string_literal("Size of decoder range cannot be zero");

    // NOTE: This implementation is shared between VP8 and VP9. Therefore, we do not check the
    //       marker bit at the start of the range decode that is required in the VP9 specification.
    //       This is instead handled by the function that instantiates all range decoders for the
    //       VP9 decoder.

    // NOTE: As noted below in fill_reservoir(), we read in multi-byte-sized chunks,
    //       so here we will deviate from the standard to count in bytes rather than bits.
    return BooleanDecoder { data.data(), data.size() };
}

// Instead of filling the value field one bit at a time as the spec suggests, we store the
// data to be read in a reservoir of greater than one byte. This allows us to read out data
// for the entire reservoir at once, avoiding a lot of branch misses in read_bool().
void BooleanDecoder::fill_reservoir()
{
    if (m_value_bits_left > 8)
        return;

    // Defer errors until the decode is finalized, so the work to check for errors and return them only has
    // to be done once. Not refilling the reservoir here will only result in reading out all zeroes until
    // the range decode is finished.
    if (m_bytes_left == 0) {
        dbgln_if(VPX_DEBUG, "BooleanDecoder has read past the end of the coded range");
        m_overread = true;
        return;
    }

    // Read the data into the most significant bits of a variable.
    auto read_size = min<size_t>(reserve_bytes, m_bytes_left);
    ValueType read_value = 0;
    memcpy(&read_value, m_data, read_size);
    read_value = AK::convert_between_host_and_big_endian(read_value);

    // Skip the number of bytes read in the data.
    m_data += read_size;
    m_bytes_left -= read_size;

    // Shift the value that was read to be less significant than the least significant bit available in the reservoir.
    read_value >>= m_value_bits_left;
    m_value |= read_value;
    m_value_bits_left += read_size * 8;
}

// 9.2.2 Boolean decoding process
bool BooleanDecoder::read_bool(u8 probability)
{
    auto split = 1u + (((m_range - 1u) * probability) >> 8u);
    // The actual value being read resides in the most significant 8 bits
    // of the value field, so we shift the split into that range for comparison.
    auto split_shifted = static_cast<ValueType>(split) << reserve_bits;
    bool return_bool;

    if (m_value < split_shifted) {
        m_range = split;
        return_bool = false;
    } else {
        m_range -= split;
        m_value -= split_shifted;
        return_bool = true;
    }

    u8 bits_to_shift_into_range = count_leading_zeroes(m_range) - ((sizeof(m_range) - 1) * 8);
    m_range <<= bits_to_shift_into_range;
    m_value <<= bits_to_shift_into_range;
    m_value_bits_left -= bits_to_shift_into_range;

    fill_reservoir();

    return return_bool;
}

// 9.2.4 Parsing process for read_literal
u8 BooleanDecoder::read_literal(u8 bits)
{
    u8 return_value = 0;
    for (size_t i = 0; i < bits; i++) {
        return_value = (2 * return_value) + read_bool(128);
    }
    return return_value;
}

ErrorOr<void> BooleanDecoder::finish_decode()
{
    if (m_overread)
        return Error::from_string_literal("Range decoder was read past the end of its data");

#if VPX_DEBUG
    // 9.2.3 Exit process for Boolean decoder
    //
    // This process is invoked when the function exit_bool( ) is called from the syntax structure.
    //
    // The padding syntax element is read using the f(BoolMaxBits) parsing process.
    //
    // It is a requirement of bitstream conformance that padding is equal to 0.
    //
    // NOTE: This requirement holds up for all of our WebP lossy test inputs, as well.
    bool padding_good = true;

    if (m_value != 0)
        padding_good = false;

    while (m_bytes_left > 0) {
        if (*m_data != 0)
            padding_good = false;
        m_data++;
        m_bytes_left--;
    }

    if (!padding_good)
        return Error::from_string_literal("Range decoder padding was non-zero");
#endif

    return {};
}

}
