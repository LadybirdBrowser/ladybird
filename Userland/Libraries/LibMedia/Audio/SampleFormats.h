/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>

namespace Audio {

// Supported PCM sample formats.
enum class PcmSampleFormat : u8 {
    Uint8,
    Int16,
    Int24,
    Int32,
    Float32,
    Float64,
};

// Most of the read code only cares about how many bits to read or write
u16 pcm_bits_per_sample(PcmSampleFormat format);
bool is_integer_format(PcmSampleFormat format);
Optional<PcmSampleFormat> integer_sample_format_for(u16 bits_per_sample);
ByteString sample_format_name(PcmSampleFormat format);
}
