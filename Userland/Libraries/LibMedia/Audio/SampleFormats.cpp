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

}
