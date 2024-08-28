/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SampleFormats.h"

namespace Audio {

u16 pcm_bits_per_sample(PcmSampleFormat format)
{
    switch (format) {
    case PcmSampleFormat::Uint8:
        return 8;
    case PcmSampleFormat::Int16:
        return 16;
    case PcmSampleFormat::Int24:
        return 24;
    case PcmSampleFormat::Int32:
    case PcmSampleFormat::Float32:
        return 32;
    case PcmSampleFormat::Float64:
        return 64;
    default:
        VERIFY_NOT_REACHED();
    }
}

bool is_integer_format(PcmSampleFormat format)
{
    return format == PcmSampleFormat::Uint8 || format == PcmSampleFormat::Int16 || format == PcmSampleFormat::Int24 || format == PcmSampleFormat::Int32;
}

Optional<PcmSampleFormat> integer_sample_format_for(u16 bits_per_sample)
{
    switch (bits_per_sample) {
    case 8:
        return PcmSampleFormat::Uint8;
    case 16:
        return PcmSampleFormat::Int16;
    case 24:
        return PcmSampleFormat::Int24;
    case 32:
        return PcmSampleFormat::Int32;
    default:
        return {};
    }
}

ByteString sample_format_name(PcmSampleFormat format)
{
    bool is_float = format == PcmSampleFormat::Float32 || format == PcmSampleFormat::Float64;
    return ByteString::formatted("PCM {}bit {}", pcm_bits_per_sample(format), is_float ? "Float" : "LE");
}

}
