/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/BinarySearch.h>
#include <AK/CharacterTypes.h>
#include <AK/Utf8View.h>
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

NonnullRefPtr<SourceCode const> SourceCode::create(String filename, Utf16String code)
{
    return adopt_ref(*new SourceCode(move(filename), move(code)));
}

NonnullRefPtr<SourceCode const> SourceCode::create(String filename, size_t length_in_code_units, String source_encoding, Core::ImmutableBytes source_bytes)
{
    return adopt_ref(*new SourceCode(move(filename), length_in_code_units, move(source_encoding), move(source_bytes)));
}

SourceCode::SourceCode(String filename, Utf16String code)
    : m_filename(move(filename))
    , m_code(move(code))
    , m_code_view(m_code->utf16_view())
    , m_length_in_code_units(m_code_view.length_in_code_units())
{
}

SourceCode::SourceCode(String filename, size_t length_in_code_units, String source_encoding, Core::ImmutableBytes source_bytes)
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
            if (all_of(source_text_bytes, AK::is_ascii))
                return Utf16String::from_ascii_without_validation(source_text_bytes);
            return Utf16String::from_utf8(StringView { source_text_bytes });
        }
        if (auto source_text = source_text_from_utf8_source_bytes(start_offset, length); source_text.has_value())
            return source_text.release_value();
        return decode_source_range(start_offset, length);
    }

    ensure_code();
    return Utf16String::from_utf16(m_code->utf16_view().substring_view(start_offset, length));
}

bool SourceCode::source_bytes_can_be_sliced_by_code_unit_offsets() const
{
    if (!m_source_bytes_can_be_sliced_by_code_unit_offsets.has_value()) {
        auto standardized_encoding = TextCodec::get_standardized_encoding(m_source_encoding);
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

Optional<Utf16String> SourceCode::source_text_from_utf8_source_bytes(size_t start_offset, size_t length) const
{
    auto start_byte_offset = byte_offset_for_utf8_code_unit_offset(start_offset);
    if (!start_byte_offset.has_value())
        return {};

    auto end_byte_offset = byte_offset_for_utf8_code_unit_offset(start_offset + length);
    if (!end_byte_offset.has_value())
        return {};

    VERIFY(*start_byte_offset <= *end_byte_offset);
    auto source_text_bytes = m_source_bytes.bytes().slice(*start_byte_offset, *end_byte_offset - *start_byte_offset);
    if (all_of(source_text_bytes, AK::is_ascii))
        return Utf16String::from_ascii_without_validation(source_text_bytes);
    return Utf16String::from_utf8(StringView { source_text_bytes });
}

bool SourceCode::ensure_utf8_source_byte_spans() const
{
    if (m_tried_to_build_utf8_source_byte_spans)
        return m_can_use_utf8_source_byte_spans;

    m_tried_to_build_utf8_source_byte_spans = true;

    auto standardized_encoding = TextCodec::get_standardized_encoding(m_source_encoding);
    if (!standardized_encoding.has_value() || !standardized_encoding->equals_ignoring_ascii_case("UTF-8"sv))
        return false;

    auto bytes = m_source_bytes.bytes();
    StringView input { bytes };

    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        input = input.substring_view(3);
        m_utf8_source_byte_span_initial_byte_offset = 3;
    } else if (bytes.size() >= 2 && ((bytes[0] == 0xFE && bytes[1] == 0xFF) || (bytes[0] == 0xFF && bytes[1] == 0xFE))) {
        return false;
    }

    auto utf8_view = Utf8View { input };
    if (!utf8_view.validate(AllowLonelySurrogates::No))
        return false;

    size_t code_unit_offset = 0;
    for (auto it = utf8_view.begin(); it != utf8_view.end(); ++it) {
        auto code_point = *it;
        size_t code_unit_length = code_point <= 0xffff ? 1 : 2;
        auto byte_length = it.underlying_code_point_length_in_bytes();
        if (byte_length != code_unit_length) {
            m_utf8_source_byte_spans.append({
                .code_unit_offset = code_unit_offset,
                .code_unit_length = code_unit_length,
                .byte_offset = m_utf8_source_byte_span_initial_byte_offset + utf8_view.byte_offset_of(it),
                .byte_length = byte_length,
            });
        }
        code_unit_offset += code_unit_length;
    }

    if (code_unit_offset != m_length_in_code_units) {
        m_utf8_source_byte_spans.clear();
        return false;
    }

    m_can_use_utf8_source_byte_spans = true;
    return true;
}

Optional<size_t> SourceCode::byte_offset_for_utf8_code_unit_offset(size_t code_unit_offset) const
{
    if (code_unit_offset > m_length_in_code_units)
        return {};

    if (!ensure_utf8_source_byte_spans())
        return {};

    size_t low = 0;
    size_t high = m_utf8_source_byte_spans.size();
    while (low < high) {
        auto middle = low + (high - low) / 2;
        auto const& span = m_utf8_source_byte_spans[middle];
        auto span_end = span.code_unit_offset + span.code_unit_length;
        if (span_end <= code_unit_offset)
            low = middle + 1;
        else
            high = middle;
    }

    if (low < m_utf8_source_byte_spans.size()) {
        auto const& span = m_utf8_source_byte_spans[low];
        if (code_unit_offset >= span.code_unit_offset && code_unit_offset < span.code_unit_offset + span.code_unit_length) {
            if (code_unit_offset == span.code_unit_offset)
                return span.byte_offset;
            return {};
        }
    }

    size_t byte_delta = m_utf8_source_byte_span_initial_byte_offset;
    if (low > 0) {
        auto const& previous_span = m_utf8_source_byte_spans[low - 1];
        auto previous_span_end_byte_offset = previous_span.byte_offset + previous_span.byte_length;
        auto previous_span_end_code_unit_offset = previous_span.code_unit_offset + previous_span.code_unit_length;
        VERIFY(previous_span_end_byte_offset >= previous_span_end_code_unit_offset);
        byte_delta = previous_span_end_byte_offset - previous_span_end_code_unit_offset;
    }

    return code_unit_offset + byte_delta;
}

Utf16String SourceCode::decode_source_range(size_t start_offset, size_t length) const
{
    if (length == 0)
        return {};

    VERIFY(start_offset <= NumericLimits<size_t>::max() - length);
    auto end_offset = start_offset + length;
    StringView input { m_source_bytes.bytes() };
    auto decoder = TextCodec::decoder_for(m_source_encoding);
    VERIFY(decoder.has_value());
    TextCodec::Decoder* actual_decoder = &decoder.value();

    auto unicode_decoder = TextCodec::bom_sniff_to_decoder(input);
    if (unicode_decoder.has_value()) {
        auto input_bytes = input.bytes();
        auto byte_order_mark_size = input_bytes.size() >= 3 && input_bytes[0] == 0xEF && input_bytes[1] == 0xBB && input_bytes[2] == 0xBF ? 3 : 2;
        actual_decoder = &unicode_decoder.value();
        input = input.substring_view(byte_order_mark_size);
    }

    StringBuilder builder(StringBuilder::Mode::UTF16, length);
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
                TRY(builder.try_append_code_unit(code_units[i]));
        }

        current_offset += code_point_length_in_code_units;
        return {};
    });
    result.release_value_but_fixme_should_propagate_errors();

    return builder.to_utf16_string();
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
