/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/StringView.h>
#include <LibTextCodec/Export.h>
#include <LibTextCodec/Forward.h>

namespace TextCodec {

class Encoder;

TEXTCODEC_API Optional<Encoder> encoder_for(StringView label);

// NB: An encoder encodes with the output encoding of its encoding, https://encoding.spec.whatwg.org/#get-an-output-encoding.
class TEXTCODEC_API Encoder {
public:
    ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(ReadonlyBytes)> on_bytes, Function<ErrorOr<void>(u32)> on_error) const;

private:
    friend TEXTCODEC_API Optional<Encoder> encoder_for(StringView);

    explicit Encoder(StringView output_encoding)
        : m_output_encoding(output_encoding)
    {
    }

    StringView m_output_encoding;
};

TEXTCODEC_API ByteString isomorphic_encode(StringView);
TEXTCODEC_API ByteString isomorphic_encode(Utf16View);

}
