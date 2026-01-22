/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/UnicodeUtils.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/WhiteSpaceProcessing.h>

namespace Web::CSS::WhiteSpaceProcessing {

namespace {

// https://drafts.csswg.org/css-text-4/#line-break-transform
// Zero-width space (U+200B) affects segment break transformation.
constexpr u32 ZWSP = 0x200B;

// https://drafts.csswg.org/css-text-4/#segment-break
// A segment break is a class of line ending characters defined by UAX14.
bool is_segment_break(u32 code_point)
{
    switch (code_point) {
    case 0x000A: // LINE FEED (LF)
    case 0x000D: // CARRIAGE RETURN (CR)
    case 0x0085: // NEXT LINE (NEL)
    case 0x2028: // LINE SEPARATOR
    case 0x2029: // PARAGRAPH SEPARATOR
        return true;
    default:
        return false;
    }
}

bool is_collapsible_space_or_tab(u32 code_point)
{
    return code_point == ' ' || code_point == '\t';
}

size_t skip_leading_spaces_and_tabs(Utf16String const& text, size_t index)
{
    size_t const length = text.length_in_code_units();
    while (index < length) {
        auto next_cp = text.code_point_at(index);
        if (!is_collapsible_space_or_tab(next_cp))
            break;
        ++index;
    }
    return index;
}

// https://drafts.csswg.org/css-text-4/#line-break-transform
bool should_remove_segment_break(Optional<u32> prev, Optional<u32> next)
{
    // "If the character immediately before or immediately after the segment break is the
    // zero-width space character (U+200B), then the break is removed, leaving behind the
    // zero-width space."
    if ((prev.has_value() && prev.value() == ZWSP) || (next.has_value() && next.value() == ZWSP))
        return true;

    // "If the East Asian Width property of both the character before and after the segment break
    // is Fullwidth, Wide, or Halfwidth (not Ambiguous), and neither side is Hangul, then the
    // segment break is removed."
    if (prev.has_value() && next.has_value()) {
        bool both_east_asian = Unicode::code_point_has_east_asian_full_half_or_wide_width(prev.value())
            && Unicode::code_point_has_east_asian_full_half_or_wide_width(next.value());
        bool either_hangul = Unicode::code_point_has_hangul_script(prev.value())
            || Unicode::code_point_has_hangul_script(next.value());

        if (both_east_asian && !either_hangul)
            return true;
    }

    return false;
}

}

Utf16String remove_collapsible_spaces_and_tabs_around_segment_breaks(Utf16String const& text)
{
    StringBuilder collapsed_builder { StringBuilder::Mode::UTF16, text.length_in_code_units() };
    StringBuilder buffered_spaces { StringBuilder::Mode::UTF16, 16 };
    size_t i = 0;
    size_t const length = text.length_in_code_units();

    while (i < length) {
        auto code_point = text.code_point_at(i);

        if (is_collapsible_space_or_tab(code_point)) {
            buffered_spaces.append_code_point(code_point);
        } else if (is_segment_break(code_point)) {
            buffered_spaces.clear();
            collapsed_builder.append_code_point('\n');
            ++i;
            i = skip_leading_spaces_and_tabs(text, i);
            continue;
        } else {
            for (auto buffered : buffered_spaces.utf16_string_view())
                collapsed_builder.append_code_point(buffered);
            buffered_spaces.clear();
            collapsed_builder.append_code_point(code_point);
        }
        ++i;
    }

    for (auto buffered : buffered_spaces.utf16_string_view())
        collapsed_builder.append_code_point(buffered);

    return collapsed_builder.to_utf16_string();
}

Utf16String collapse_consecutive_segment_breaks(Utf16String const& text)
{
    StringBuilder deduped_builder { StringBuilder::Mode::UTF16, text.length_in_code_units() };
    bool last_was_segment_break = false;
    for (auto code_point : text) {
        if (is_segment_break(code_point) && !last_was_segment_break) {
            deduped_builder.append_code_point('\n');
            last_was_segment_break = true;
        } else if (!is_segment_break(code_point)) {
            deduped_builder.append_code_point(code_point);
            last_was_segment_break = false;
        }
    }
    return deduped_builder.to_utf16_string();
}

Utf16String transform_segment_breaks_for_collapse(Utf16String const& text)
{
    StringBuilder transformed_builder { StringBuilder::Mode::UTF16, text.length_in_code_units() };
    Optional<u32> previous_code_point;
    auto length = text.length_in_code_units();

    for (size_t i = 0; i < length;) {
        auto code_point = text.code_point_at(i);
        auto code_unit_length = AK::UnicodeUtils::code_unit_length_for_code_point(code_point);

        if (is_segment_break(code_point)) {
            Optional<u32> next_cp;
            if (i + code_unit_length < length)
                next_cp = text.code_point_at(i + code_unit_length);

            if (!should_remove_segment_break(previous_code_point, next_cp)) {
                transformed_builder.append_code_point(' ');
                previous_code_point = ' ';
            }

            i += code_unit_length;
            continue;
        }

        transformed_builder.append_code_point(code_point);
        previous_code_point = code_point;
        i += code_unit_length;
    }

    return transformed_builder.to_utf16_string();
}

}
