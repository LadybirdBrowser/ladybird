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

// https://encoding.spec.whatwg.org/#concept-encoding-error-mode
enum class ErrorMode {
    Replacement,
    Fatal,
};

class TEXTCODEC_API Decoder {
public:
    virtual ErrorOr<String> to_utf8(StringView, IgnoreBOM, ErrorMode);
    virtual ErrorOr<Utf16String> to_utf16(StringView);
    virtual ErrorOr<size_t> length_in_utf16_code_units(StringView);
    ErrorOr<void> process_code_points(StringView, Function<ErrorOr<void>(u32)>);

protected:
    virtual ~Decoder() = default;
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) = 0;
};

class TEXTCODEC_API StreamingDecoder final {
    AK_MAKE_NONCOPYABLE(StreamingDecoder);

public:
    StreamingDecoder(StringView encoding, IgnoreBOM, ErrorMode);
    ~StreamingDecoder();

    ErrorOr<String> to_utf8(ReadonlyBytes);
    ErrorOr<String> finish();

private:
    ErrorMode m_error_mode { ErrorMode::Replacement };
    void* m_decoder { nullptr };
};

// This will return a decoder for the exact name specified, skipping get_standardized_encoding.
// Use this when you want ISO-8859-1 instead of windows-1252.
TEXTCODEC_API Optional<Decoder&> decoder_for_exact_name(StringView encoding);

TEXTCODEC_API Optional<Decoder&> decoder_for(StringView encoding);
TEXTCODEC_API Optional<StringView> get_standardized_encoding(StringView encoding);

// This returns the appropriate Unicode decoder for the sniffed BOM or nothing if there is no appropriate decoder.
TEXTCODEC_API Optional<Decoder&> bom_sniff_to_decoder(StringView);

// NOTE: This has an obnoxious name to discourage usage. Only use this if you absolutely must! For example, XHR in LibWeb uses this.
// This will use the given decoder unless there is a byte order mark in the input, in which we will instead use the appropriate Unicode decoder.
TEXTCODEC_API ErrorOr<String> convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder&, StringView);
TEXTCODEC_API ErrorOr<Utf16String> convert_input_to_utf16_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder&, StringView);
TEXTCODEC_API ErrorOr<size_t> convert_input_to_utf16_length_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder&, StringView);

TEXTCODEC_API StringView get_output_encoding(StringView encoding);

TEXTCODEC_API String isomorphic_decode(StringView);

}
