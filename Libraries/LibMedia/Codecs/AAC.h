/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>

namespace Media::Codecs {

class AAC {
public:
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
};

}
