/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::Codecs {

class H264 {
public:
    struct Parameters {
        u8 profile_idc { 0 };
        u8 constraint_set_flags { 0 };
        u8 level_idc { 0 };

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes);
};

}
