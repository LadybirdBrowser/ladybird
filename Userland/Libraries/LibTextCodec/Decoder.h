/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>

namespace TextCodec {

class Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) = 0;
    virtual bool validate(StringView);
    virtual ErrorOr<String> to_utf8(StringView);

protected:
    virtual ~Decoder() = default;
};

class UTF8Decoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView) override;
    virtual ErrorOr<String> to_utf8(StringView) override;
};

class UTF16BEDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView) override;
    virtual ErrorOr<String> to_utf8(StringView) override;
};

class UTF16LEDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView) override;
    virtual ErrorOr<String> to_utf8(StringView) override;
};

template<Integral ArrayType = u32>
class SingleByteDecoder final : public Decoder {
public:
    SingleByteDecoder(Array<ArrayType, 128> translation_table)
        : m_translation_table(translation_table)
    {
    }

    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;

private:
    Array<ArrayType, 128> m_translation_table;
};

class Latin1Decoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView) override { return true; }
};

class PDFDocEncodingDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView) override { return true; }
};

class XUserDefinedDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView) override { return true; }
};

class GB18030Decoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
};

class Big5Decoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
};

class EUCJPDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
};

class ISO2022JPDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
};

class ShiftJISDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
};

class EUCKRDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
};

class ReplacementDecoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual bool validate(StringView input) override { return input.is_empty(); }
};

// This will return a decoder for the exact name specified, skipping get_standardized_encoding.
// Use this when you want ISO-8859-1 instead of windows-1252.
Optional<Decoder&> decoder_for_exact_name(StringView encoding);

Optional<Decoder&> decoder_for(StringView encoding);
Optional<StringView> get_standardized_encoding(StringView encoding);

// This returns the appropriate Unicode decoder for the sniffed BOM or nothing if there is no appropriate decoder.
Optional<Decoder&> bom_sniff_to_decoder(StringView);

// NOTE: This has an obnoxious name to discourage usage. Only use this if you absolutely must! For example, XHR in LibWeb uses this.
// This will use the given decoder unless there is a byte order mark in the input, in which we will instead use the appropriate Unicode decoder.
ErrorOr<String> convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder&, StringView);

StringView get_output_encoding(StringView encoding);

}
