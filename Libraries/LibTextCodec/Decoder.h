/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibTextCodec/Export.h>
#include <LibTextCodec/Forward.h>

namespace TextCodec {

enum class IgnoreBOM {
    Yes,
    No,
};

enum class Flush {
    No,
    Yes,
};

// https://encoding.spec.whatwg.org/#concept-encoding-error-mode
enum class ErrorMode {
    Replacement,
    Fatal,
};

class Decoder;

TEXTCODEC_API Optional<Decoder> decoder_for(StringView encoding);
TEXTCODEC_API Optional<Decoder> decoder_for(Utf16View encoding);

// NB: A decoder never removes a BOM. The decode algorithms below do that.
class TEXTCODEC_API Decoder {
public:
    ErrorOr<String> to_utf8(ReadonlyBytes, ErrorMode = ErrorMode::Replacement) const;
    ErrorOr<Utf16String> to_utf16(ReadonlyBytes) const;
    ErrorOr<size_t> length_in_utf16_code_units(ReadonlyBytes) const;

private:
    friend TEXTCODEC_API Optional<Decoder> decoder_for(StringView);
    friend TEXTCODEC_API Optional<Decoder> decoder_for(Utf16View);

    explicit Decoder(StringView encoding)
        : m_encoding(encoding)
    {
    }

    StringView m_encoding;
};

class TEXTCODEC_API StreamingDecoder final {
    AK_MAKE_NONCOPYABLE(StreamingDecoder);

public:
    AK_ALLOC_WITH_KMALLOC;

    StreamingDecoder(StringView encoding, IgnoreBOM, ErrorMode);
    ~StreamingDecoder();

    ErrorOr<Utf16String> decode(ReadonlyBytes, Flush = Flush::No);

private:
    void* m_decoder { nullptr };
};

TEXTCODEC_API Optional<StringView> get_standardized_encoding(StringView encoding);
TEXTCODEC_API Optional<StringView> get_standardized_encoding(Utf16View encoding);

// https://encoding.spec.whatwg.org/#decode
TEXTCODEC_API ErrorOr<String> decode_to_utf8(ReadonlyBytes, Decoder fallback_decoder, ErrorMode = ErrorMode::Replacement);
TEXTCODEC_API ErrorOr<Utf16String> decode_to_utf16(ReadonlyBytes, Decoder fallback_decoder);
TEXTCODEC_API ErrorOr<size_t> decoded_length_in_utf16_code_units(ReadonlyBytes, Decoder fallback_decoder);

// https://encoding.spec.whatwg.org/#utf-8-decode
TEXTCODEC_API ErrorOr<Utf16String> utf8_decode_to_utf16(ReadonlyBytes);

TEXTCODEC_API StringView get_output_encoding(StringView encoding);

TEXTCODEC_API String isomorphic_decode(StringView);
TEXTCODEC_API Utf16String isomorphic_decode_to_utf16(StringView);

}
