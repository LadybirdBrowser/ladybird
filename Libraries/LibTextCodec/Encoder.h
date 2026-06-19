/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <LibTextCodec/Export.h>
#include <LibTextCodec/Forward.h>

namespace TextCodec {

class TEXTCODEC_API Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) = 0;

protected:
    virtual ~Encoder() = default;
};

TEXTCODEC_API Optional<Encoder&> encoder_for_exact_name(StringView encoding);
TEXTCODEC_API Optional<Encoder&> encoder_for(StringView label);

TEXTCODEC_API ByteString isomorphic_encode(StringView);

}
