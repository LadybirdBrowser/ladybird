/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/BitReader.h>
#include <LibMedia/Codecs/AAC.h>

namespace Media::Codecs {

Optional<AAC::Parameters> AAC::parse_configuration_record(ReadonlyBytes audio_specific_config, u8 object_type_indication)
{
    static constexpr u8 ESCAPED_AUDIO_OBJECT_TYPE = 31;

    BitReader reader { audio_specific_config };
    u32 audio_object_type = reader.read_bits<u8>(5);
    if (audio_object_type == ESCAPED_AUDIO_OBJECT_TYPE)
        audio_object_type = 32 + reader.read_bits<u8>(6);
    if (reader.has_overrun())
        return {};

    return Parameters { object_type_indication, audio_object_type };
}

}
