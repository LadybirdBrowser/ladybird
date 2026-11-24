/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibUnicode/Bidi.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/TextMapping.h>
#include <unicode/ubidi.h>
#include <unicode/utf16.h>

static constexpr u8 resets_at_line_end_mask = 0x80;
static constexpr u8 paragraph_separator_level = 0xff;
static_assert(UBIDI_MAX_EXPLICIT_LEVEL + 1 < (paragraph_separator_level & ~resets_at_line_end_mask));

namespace Unicode {

bool may_require_bidi_processing(Utf16View const& text)
{
    // NB: No code point up to U+00FF is right to left.
    if (text.has_latin1_storage())
        return false;

    for (auto code_point : text) {
        switch (bidirectional_class(code_point)) {
        case BidiClass::ArabicNumber:
        case BidiClass::RightToLeft:
        case BidiClass::RightToLeftArabic:
        case BidiClass::RightToLeftEmbedding:
        case BidiClass::RightToLeftIsolate:
        case BidiClass::RightToLeftOverride:
        case BidiClass::LeftToRightEmbedding:
        case BidiClass::LeftToRightIsolate:
        case BidiClass::LeftToRightOverride:
        case BidiClass::FirstStrongIsolate:
        case BidiClass::PopDirectionalFormat:
        case BidiClass::PopDirectionalIsolate:
            return true;
        default:
            break;
        }
    }

    return false;
}

}

extern "C" bool unicode_text_may_require_bidi_processing(u16 const* text, size_t length)
{
    return Unicode::may_require_bidi_processing({ reinterpret_cast<char16_t const*>(text), length });
}

extern "C" size_t unicode_bidi_find_paragraph_separator(u16 const* text, size_t length)
{
    Utf16View view { reinterpret_cast<char16_t const*>(text), length };
    for (auto iterator = view.begin(); iterator != view.end(); ++iterator) {
        if (u_charDirection(static_cast<UChar32>(*iterator)) == U_BLOCK_SEPARATOR)
            return view.iterator_offset(iterator);
    }
    return length;
}

extern "C" UnicodeBidiParagraph unicode_bidi_resolve_paragraph(u16 const* text, i32 length, u8 base_level, bool first_strong, u8* levels)
{
    auto status = U_ZERO_ERROR;
    auto* bidi = ubidi_open();
    VERIFY(bidi);
    auto const* utf16 = reinterpret_cast<UChar const*>(text);
    ubidi_setPara(bidi, utf16, length, first_strong ? UBIDI_DEFAULT_LTR : base_level, nullptr, &status);
    auto const* resolved = ubidi_getLevels(bidi, &status);
    VERIFY(U_SUCCESS(status));

    bool has_isolates = false;
    // Retain the original whitespace and formatting types for L1 after the paragraph is wrapped.
    for (i32 offset = 0; offset < length;) {
        auto start = offset;
        UChar32 code_point;
        U16_NEXT(utf16, offset, length, code_point);
        auto const type = u_charDirection(code_point);
        bool const opens_isolate = type == U_LEFT_TO_RIGHT_ISOLATE || type == U_RIGHT_TO_LEFT_ISOLATE || type == U_FIRST_STRONG_ISOLATE;
        has_isolates |= opens_isolate;
        bool const resets_at_line_end = type == U_WHITE_SPACE_NEUTRAL || type == U_SEGMENT_SEPARATOR
            || opens_isolate || type == U_POP_DIRECTIONAL_ISOLATE || type == U_BOUNDARY_NEUTRAL
            || type == U_LEFT_TO_RIGHT_EMBEDDING || type == U_RIGHT_TO_LEFT_EMBEDDING
            || type == U_LEFT_TO_RIGHT_OVERRIDE || type == U_RIGHT_TO_LEFT_OVERRIDE || type == U_POP_DIRECTIONAL_FORMAT;
        for (auto i = start; i < offset; ++i) {
            levels[i] = type == U_BLOCK_SEPARATOR
                ? paragraph_separator_level
                : resolved[i] | (resets_at_line_end ? resets_at_line_end_mask : 0);
        }
    }

    auto const paragraph_level = ubidi_getParaLevel(bidi);
    bool has_strong_direction = first_strong && (paragraph_level != 0 || ubidi_getBaseDirection(utf16, length) != UBIDI_NEUTRAL);
    if (has_strong_direction && paragraph_level == 0 && has_isolates) {
        // ubidi_getBaseDirection also sees strong text inside isolates. Try the opposite default to determine
        // whether ICU chose LTR from strong text outside the isolates. The levels above keep the LTR default.
        ubidi_setPara(bidi, utf16, length, UBIDI_DEFAULT_RTL, nullptr, &status);
        VERIFY(U_SUCCESS(status));
        has_strong_direction = ubidi_getParaLevel(bidi) == 0;
    }
    ubidi_close(bidi);
    return { paragraph_level, has_strong_direction };
}

extern "C" i32 unicode_bidi_reorder_line(u8 const* paragraph_levels, i32 length, u8 base_level, i32* indices)
{
    // UAX #9 rule L1 resets trailing whitespace and formatting characters after line breaking.
    auto trailing_start = length;
    while (trailing_start > 0 && (paragraph_levels[trailing_start - 1] & resets_at_line_end_mask))
        --trailing_start;

    Vector<UBiDiLevel, 128> line_levels;
    line_levels.resize(length);
    for (i32 index = 0; index < trailing_start; ++index) {
        auto level = paragraph_levels[index];
        line_levels[index] = level == paragraph_separator_level ? base_level : level & ~resets_at_line_end_mask;
    }
    for (auto index = trailing_start; index < length; ++index)
        line_levels[index] = base_level;

    ubidi_reorderLogical(line_levels.data(), length, indices);
    return trailing_start;
}
