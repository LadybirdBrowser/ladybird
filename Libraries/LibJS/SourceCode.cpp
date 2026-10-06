/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/UnicodeUtils.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/ScriptAndModuleABIConversions.h>
#include <LibJS/SourceCode.h>
#include <LibTextCodec/Decoder.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(sizeof(Utf16String) == sizeof(JSOwnedUtf16String));
static_assert(alignof(Utf16String) == alignof(JSOwnedUtf16String));

// The source code's string, which the runtime keeps in an AK::Utf16String of its own for as long as the source code
// lives.
static Utf16String const& string_in_place_from_abi(JSOwnedUtf16String const* string)
{
    return *reinterpret_cast<Utf16String const*>(string);
}

// What SourceCode decodes its source bytes to: the code units in the range, with the encoding of a byte order mark
// taking precedence over the given one.
static Utf16String decode_source_bytes(ReadonlyBytes source_bytes, StringView source_encoding, size_t length_in_code_units)
{
    if (length_in_code_units == 0)
        return {};

    StringView input { source_bytes };
    auto decoder = TextCodec::decoder_for(source_encoding);
    VERIFY(decoder.has_value());
    TextCodec::Decoder* actual_decoder = &decoder.value();

    auto unicode_decoder = TextCodec::bom_sniff_to_decoder(input);
    if (unicode_decoder.has_value()) {
        auto input_bytes = input.bytes();
        auto byte_order_mark_size = input_bytes.size() >= 3 && input_bytes[0] == 0xEF && input_bytes[1] == 0xBB && input_bytes[2] == 0xBF ? 3 : 2;
        actual_decoder = &unicode_decoder.value();
        input = input.substring_view(byte_order_mark_size);
    }

    Utf16StringBuilder builder(length_in_code_units);
    size_t current_offset = 0;
    auto result = actual_decoder->process_code_points(input, [&](auto code_point) -> ErrorOr<void> {
        char16_t code_units[2];
        size_t code_point_length_in_code_units = 0;
        (void)AK::UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
            code_units[code_point_length_in_code_units++] = code_unit;
        });

        for (size_t i = 0; i < code_point_length_in_code_units; ++i) {
            if (current_offset + i < length_in_code_units)
                builder.append_code_unit(code_units[i]);
        }

        current_offset += code_point_length_in_code_units;
        return {};
    });
    result.release_value_but_fixme_should_propagate_errors();

    return builder.to_string();
}

NonnullRefPtr<SourceCode const> SourceCode::create(Utf16String filename, Utf16String code)
{
    return adopt_source_code_from_abi(js_source_code_create(owned_utf16_string_to_abi(move(filename)), owned_utf16_string_to_abi(move(code))));
}

NonnullRefPtr<SourceCode const> SourceCode::create(Utf16String filename, size_t length_in_code_units, ByteString source_encoding, Core::ImmutableBytes source_bytes)
{
    return create(move(filename), decode_source_bytes(source_bytes.bytes(), source_encoding.view(), length_in_code_units));
}

Utf16String const& SourceCode::filename() const
{
    return string_in_place_from_abi(js_source_code_filename_address(source_code_to_abi(*this)));
}

Utf16String const& SourceCode::code() const
{
    return string_in_place_from_abi(js_source_code_code_address(source_code_to_abi(*this)));
}

size_t SourceCode::length_in_code_units() const
{
    return js_source_code_length_in_code_units(source_code_to_abi(*this));
}

u16 const* SourceCode::utf16_data() const
{
    return js_source_code_utf16_data(source_code_to_abi(*this));
}

Utf16String SourceCode::source_text_from_offsets(size_t start_offset, size_t length) const
{
    if (length == 0)
        return {};

    VERIFY(start_offset <= NumericLimits<size_t>::max() - length);
    return Utf16String::from_utf16(code().utf16_view().substring_view(start_offset, length));
}

void SourceCode::ref() const
{
    js_source_code_retain(source_code_to_abi(*this));
}

void SourceCode::unref() const
{
    js_source_code_release(source_code_to_abi(*this));
}

}
