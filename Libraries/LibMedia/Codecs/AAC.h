/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::Codecs {

class AAC {
public:
    static constexpr u8 MPEG4_AUDIO_OBJECT_TYPE_INDICATION = 0x40;

    struct Parameters {
        u8 object_type_indication;
        Optional<u32> audio_object_type;

        constexpr bool is_fully_specified() const
        {
            // The MPEG-4 Audio object type indication identifies a family of audio object types,
            // so the Audio Object Type is also required.
            if (object_type_indication == 0x40)
                return audio_object_type.has_value();

            return true;
        }

        bool operator==(Parameters const&) const = default;
    };

    // The Audio Specific Config carries no object type indication, so callers supply the one their container implies:
    // the descriptor's own value for ISOBMFF, and MPEG-4 Audio for Matroska's A_AAC.
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes audio_specific_config, u8 object_type_indication);
};

}
