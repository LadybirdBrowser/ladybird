/*
 * Copyright (c) 2022-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Emoji.h>

namespace Unicode {

Optional<Emoji> __attribute__((weak)) find_emoji_for_code_points(ReadonlySpan<u32>) { return {}; }

// https://unicode.org/reports/tr51/#def_emoji_core_sequence
static bool could_be_start_of_emoji_core_sequence(u32 code_point, Optional<u32> const& next_code_point, SequenceType type)
{
    // emoji_core_sequence := emoji_character | emoji_presentation_sequence | emoji_keycap_sequence | emoji_modifier_sequence | emoji_flag_sequence

    static constexpr auto emoji_presentation_selector = 0xFE0Fu;
    static constexpr auto combining_enclosing_keycap = 0x20E3u;
    static constexpr auto zero_width_joiner = 0x200Du;

    // https://unicode.org/reports/tr51/#def_emoji_keycap_sequence
    // emoji_keycap_sequence := [0-9#*] \x{FE0F 20E3}
    if (is_ascii_digit(code_point) || code_point == '#' || code_point == '*')
        return next_code_point == emoji_presentation_selector || next_code_point == combining_enclosing_keycap;

    // A little non-standard, but all other ASCII code points are not the beginning of any emoji sequence.
    if (is_ascii(code_point))
        return false;

    // https://unicode.org/reports/tr51/#def_emoji_character
    switch (type) {
    case SequenceType::Any:
        if (code_point_has_emoji_property(code_point))
            return true;
        break;
    case SequenceType::EmojiPresentation:
        if (code_point_has_emoji_presentation_property(code_point))
            return true;
        if (next_code_point == zero_width_joiner && code_point_has_emoji_property(code_point))
            return true;
        break;
    }

    // https://unicode.org/reports/tr51/#def_emoji_presentation_sequence
    // emoji_presentation_sequence := emoji_character emoji_presentation_selector
    if (next_code_point == emoji_presentation_selector)
        return true;

    // https://unicode.org/reports/tr51/#def_emoji_modifier_sequence
    // emoji_modifier_sequence := emoji_modifier_base emoji_modifier
    if (code_point_has_emoji_modifier_base_property(code_point))
        return true;

    // https://unicode.org/reports/tr51/#def_emoji_flag_sequence
    // emoji_flag_sequence := regional_indicator regional_indicator
    if (code_point_has_regional_indicator_property(code_point))
        return true;

    return false;
}

static bool could_be_start_of_serenity_emoji(u32 code_point)
{
    // We use Supplementary Private Use Area-B for custom Serenity emoji, starting at U+10CD00.
    static constexpr auto first_custom_serenity_emoji_code_point = 0x10CD00u;

    return code_point >= first_custom_serenity_emoji_code_point;
}

// https://unicode.org/reports/tr51/#def_emoji_sequence
template<typename CodePointIterator>
static bool could_be_start_of_emoji_sequence_impl(CodePointIterator const& it, SequenceType type)
{
    // emoji_sequence := emoji_core_sequence | emoji_zwj_sequence | emoji_tag_sequence

    if (it.done())
        return false;

    // The purpose of this method is to quickly filter out code points that cannot be the start of
    // an emoji. The emoji_core_sequence definition alone captures the start of all possible
    // emoji_zwj_sequence and emoji_tag_sequence emojis, because:
    //
    //     * emoji_zwj_sequence must begin with emoji_zwj_element, which is:
    //       emoji_zwj_element := emoji_core_sequence | emoji_tag_sequence
    //
    //     * emoji_tag_sequence must begin with tag_base, which is:
    //       tag_base := emoji_character | emoji_modifier_sequence | emoji_presentation_sequence
    //       Note that this is a subset of emoji_core_sequence.
    auto code_point = *it;
    auto next_code_point = it.peek(1);

    if (could_be_start_of_emoji_core_sequence(code_point, next_code_point, type))
        return true;
    if (could_be_start_of_serenity_emoji(code_point))
        return true;
    return false;
}

bool could_be_start_of_emoji_sequence(Utf8CodePointIterator const& it, SequenceType type)
{
    return could_be_start_of_emoji_sequence_impl(it, type);
}

bool could_be_start_of_emoji_sequence(Utf32CodePointIterator const& it, SequenceType type)
{
    return could_be_start_of_emoji_sequence_impl(it, type);
}

}
