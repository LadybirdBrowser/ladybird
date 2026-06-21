/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/BinarySearch.h>
#include <AK/CharacterTypes.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/SourceCode.h>
#include <LibJS/SourceRange.h>
#include <LibJS/Token.h>
#include <LibTextCodec/Decoder.h>

namespace JS {

static bool ascii_source_bytes_decode_to_same_code_units(StringView standardized_encoding, ReadonlyBytes bytes)
{
    auto decoder = TextCodec::decoder_for_exact_name(standardized_encoding);
    if (!decoder.has_value())
        return false;

    size_t byte_offset = 0;
    bool bytes_are_identity_mapped = true;
    auto result = decoder->process_code_points(StringView { bytes }, [&](auto code_point) -> ErrorOr<void> {
        if (byte_offset >= bytes.size()) {
            bytes_are_identity_mapped = false;
            return {};
        }

        auto byte = bytes[byte_offset++];
        if (code_point != byte)
            bytes_are_identity_mapped = false;
        return {};
    });
    result.release_value_but_fixme_should_propagate_errors();

    return bytes_are_identity_mapped && byte_offset == bytes.size();
}

NonnullRefPtr<SourceCode const> SourceCode::create(Utf16String filename, Utf16String code)
{
    return adopt_ref(*new SourceCode(move(filename), move(code)));
}

NonnullRefPtr<SourceCode const> SourceCode::create(Utf16String filename, size_t length_in_code_units, ByteString source_encoding, Core::ImmutableBytes source_bytes)
{
    return adopt_ref(*new SourceCode(move(filename), length_in_code_units, move(source_encoding), move(source_bytes)));
}

SourceCode::SourceCode(Utf16String filename, Utf16String code)
    : m_filename(move(filename))
    , m_code(move(code))
    , m_code_view(m_code->utf16_view())
    , m_length_in_code_units(m_code_view.length_in_code_units())
{
}

SourceCode::SourceCode(Utf16String filename, size_t length_in_code_units, ByteString source_encoding, Core::ImmutableBytes source_bytes)
    : m_filename(move(filename))
    , m_source_encoding(move(source_encoding))
    , m_source_bytes(move(source_bytes))
    , m_length_in_code_units(length_in_code_units)
{
}

void SourceCode::ensure_code() const
{
    if (m_code.has_value())
        return;

    m_code = decode_source_range(0, m_length_in_code_units);
    m_source_bytes = {};
    m_code_view = m_code->utf16_view();
}

Utf16String const& SourceCode::code() const
{
    ensure_code();
    return *m_code;
}

Utf16View const& SourceCode::code_view() const
{
    ensure_code();
    return m_code_view;
}

u16 const* SourceCode::utf16_data() const
{
    ensure_code();

    if (!m_code_view.has_ascii_storage())
        return reinterpret_cast<u16 const*>(m_code_view.utf16_span().data());

    if (m_utf16_data_cache.is_empty() && m_length_in_code_units > 0) {
        auto ascii = m_code_view.ascii_span();
        m_utf16_data_cache.ensure_capacity(m_length_in_code_units);
        for (size_t i = 0; i < m_length_in_code_units; ++i)
            m_utf16_data_cache.unchecked_append(static_cast<u16>(ascii[i]));
    }
    return m_utf16_data_cache.data();
}

Utf16String SourceCode::source_text_from_offsets(size_t start_offset, size_t length) const
{
    if (length == 0)
        return {};

    VERIFY(start_offset <= NumericLimits<size_t>::max() - length);

    if (m_code.has_value())
        return Utf16String::from_utf16(m_code->utf16_view().substring_view(start_offset, length));

    if (m_source_bytes.is_valid()) {
        if (source_bytes_can_be_sliced_by_code_unit_offsets()) {
            auto bytes = m_source_bytes.bytes();
            VERIFY(m_length_in_code_units == bytes.size());
            auto source_text_bytes = bytes.slice(start_offset, length);
            VERIFY(all_of(source_text_bytes, AK::is_ascii));
            return Utf16String::from_ascii_without_validation(source_text_bytes);
        }
        return decode_source_range(start_offset, length);
    }

    ensure_code();
    return Utf16String::from_utf16(m_code->utf16_view().substring_view(start_offset, length));
}

bool SourceCode::source_bytes_can_be_sliced_by_code_unit_offsets() const
{
    if (!m_source_bytes_can_be_sliced_by_code_unit_offsets.has_value()) {
        auto standardized_encoding = TextCodec::get_standardized_encoding(m_source_encoding.view());
        if (!standardized_encoding.has_value()) {
            m_source_bytes_can_be_sliced_by_code_unit_offsets = false;
            return *m_source_bytes_can_be_sliced_by_code_unit_offsets;
        }

        auto bytes = m_source_bytes.bytes();
        if (m_length_in_code_units != bytes.size()) {
            m_source_bytes_can_be_sliced_by_code_unit_offsets = false;
            return *m_source_bytes_can_be_sliced_by_code_unit_offsets;
        }

        auto source_bytes_are_ascii = all_of(bytes, AK::is_ascii);
        if (standardized_encoding->equals_ignoring_ascii_case("UTF-8"sv)) {
            m_source_bytes_can_be_sliced_by_code_unit_offsets = source_bytes_are_ascii;
            return *m_source_bytes_can_be_sliced_by_code_unit_offsets;
        }

        if (!source_bytes_are_ascii) {
            m_source_bytes_can_be_sliced_by_code_unit_offsets = false;
            return *m_source_bytes_can_be_sliced_by_code_unit_offsets;
        }

        m_source_bytes_can_be_sliced_by_code_unit_offsets = ascii_source_bytes_decode_to_same_code_units(*standardized_encoding, bytes);
    }

    return *m_source_bytes_can_be_sliced_by_code_unit_offsets;
}

Utf16String SourceCode::decode_source_range(size_t start_offset, size_t length) const
{
    if (length == 0)
        return {};

    VERIFY(start_offset <= NumericLimits<size_t>::max() - length);
    auto end_offset = start_offset + length;
    StringView input { m_source_bytes.bytes() };
    auto decoder = TextCodec::decoder_for(m_source_encoding.view());
    VERIFY(decoder.has_value());
    TextCodec::Decoder* actual_decoder = &decoder.value();

    auto unicode_decoder = TextCodec::bom_sniff_to_decoder(input);
    if (unicode_decoder.has_value()) {
        auto input_bytes = input.bytes();
        auto byte_order_mark_size = input_bytes.size() >= 3 && input_bytes[0] == 0xEF && input_bytes[1] == 0xBB && input_bytes[2] == 0xBF ? 3 : 2;
        actual_decoder = &unicode_decoder.value();
        input = input.substring_view(byte_order_mark_size);
    }

    Utf16StringBuilder builder(length);
    size_t current_offset = 0;
    auto result = actual_decoder->process_code_points(input, [&](auto code_point) -> ErrorOr<void> {
        char16_t code_units[2];
        size_t code_point_length_in_code_units = 0;
        (void)AK::UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
            code_units[code_point_length_in_code_units++] = code_unit;
        });

        for (size_t i = 0; i < code_point_length_in_code_units; ++i) {
            auto code_unit_offset = current_offset + i;
            if (code_unit_offset >= start_offset && code_unit_offset < end_offset)
                builder.append_code_unit(code_units[i]);
        }

        current_offset += code_point_length_in_code_units;
        return {};
    });
    result.release_value_but_fixme_should_propagate_errors();

    return builder.to_string();
}

void SourceCode::fill_position_cache() const
{
    constexpr size_t predicted_minimum_cached_positions = 8;
    constexpr size_t minimum_distance_between_cached_positions = 32;
    constexpr size_t maximum_distance_between_cached_positions = 8192;

    auto const& code = this->code();
    if (code.is_empty())
        return;

    u32 previous_code_point = 0;
    u32 line = 1;
    u32 column = 1;
    u32 offset_of_last_starting_point = 0;

    m_cached_positions.ensure_capacity(predicted_minimum_cached_positions + (code.length_in_code_units() / maximum_distance_between_cached_positions));
    m_cached_positions.append({ .position = { .line = 1, .column = 1 }, .offset = 0 });

    auto view = code.utf16_view();

    for (auto it = view.begin(); it != view.end(); ++it) {
        u32 code_point = *it;
        bool is_line_terminator = code_point == '\r' || (code_point == '\n' && previous_code_point != '\r') || code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;

        auto offset = view.iterator_offset(it);
        VERIFY(offset <= NumericLimits<u32>::max());

        bool is_nonempty_line = is_line_terminator && previous_code_point != '\n' && previous_code_point != LINE_SEPARATOR && previous_code_point != PARAGRAPH_SEPARATOR && (code_point == '\n' || previous_code_point != '\r');
        auto distance_between_cached_position = offset - offset_of_last_starting_point;

        if ((distance_between_cached_position >= minimum_distance_between_cached_positions && is_nonempty_line) || distance_between_cached_position >= maximum_distance_between_cached_positions) {
            m_cached_positions.append({ .position = { .line = line, .column = column }, .offset = static_cast<u32>(offset) });
            offset_of_last_starting_point = offset;
        }

        if (is_line_terminator) {
            line += 1;
            column = 1;
        } else {
            column += 1;
        }

        previous_code_point = code_point;
    }
}

SourceRange SourceCode::range_from_offsets(u32 start_offset, [[maybe_unused]] u32 end_offset) const
{
    // If the underlying code is an empty string, the range is 1,1 no matter what.
    auto const& code = this->code();
    if (code.is_empty())
        return { *this, { .line = 1, .column = 1 } };

    if (m_cached_positions.is_empty())
        fill_position_cache();

    CachedPosition current { .position = { .line = 1, .column = 1 }, .offset = 0 };

    if (!m_cached_positions.is_empty()) {
        CachedPosition const dummy;
        size_t nearest_index = 0;
        binary_search(m_cached_positions, dummy, &nearest_index,
            [&](auto&, auto& starting_point) {
                return start_offset - starting_point.offset;
            });

        current = m_cached_positions[nearest_index];
    }

    Optional<Position> start;

    u32 previous_code_point = 0;

    auto view = code.utf16_view();

    for (auto it = view.iterator_at_code_unit_offset(current.offset); it != view.end(); ++it) {
        // If we're on or after the start offset, this is the start position.
        if (!start.has_value() && view.iterator_offset(it) >= start_offset) {
            start = Position {
                .line = current.position.line,
                .column = current.position.column,
            };
        }

        u32 code_point = *it;

        bool const is_line_terminator = code_point == '\r' || (code_point == '\n' && previous_code_point != '\r') || code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;
        previous_code_point = code_point;

        if (is_line_terminator) {
            current.position.line += 1;
            current.position.column = 1;
            continue;
        }

        current.position.column += 1;
    }

    // If we didn't find a start position, just return 1,1.
    // FIXME: This is a hack. Find a way to return the nicest possible values here.
    if (!start.has_value())
        return SourceRange { *this, { .line = 1, .column = 1 } };

    return SourceRange { *this, *start };
}

}
