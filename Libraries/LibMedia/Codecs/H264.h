/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Optional.h>

namespace Media::Codecs {

class H264 {
public:
    struct Parameters {
        u8 profile_idc;
        u8 constraint_set_flags;
        u8 level_idc;

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
};

}
