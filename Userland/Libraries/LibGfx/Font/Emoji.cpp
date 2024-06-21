/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Span.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>
#include <AK/Variant.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Emoji.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Emoji.h>

namespace Gfx {

// https://unicode.org/reports/tr51/
// https://unicode.org/emoji/charts/emoji-list.html
// https://unicode.org/emoji/charts/emoji-zwj-sequences.html

static HashMap<StringView, RefPtr<Gfx::Bitmap>> s_emojis;
static Variant<String, StringView> s_emoji_lookup_path = "/res/emoji"sv;

static StringView emoji_lookup_path()
{
    return s_emoji_lookup_path.visit([](auto const& path) -> StringView { return path; });
}

void Emoji::set_emoji_lookup_path(String emoji_lookup_path)
{
    s_emoji_lookup_path = move(emoji_lookup_path);
}

Bitmap const* Emoji::emoji_for_code_point(u32 code_point)
{
    return emoji_for_code_points(Array { code_point });
}

Bitmap const* Emoji::emoji_for_code_points(ReadonlySpan<u32> const& code_points)
{
    auto emoji_file = Unicode::emoji_image_for_code_points(code_points);
    if (!emoji_file.has_value())
        return nullptr;

    if (auto it = s_emojis.find(*emoji_file); it != s_emojis.end())
        return it->value.ptr();

    auto emoji_path = LexicalPath::join(emoji_lookup_path(), *emoji_file);
    auto bitmap_or_error = Bitmap::load_from_file(emoji_path.string());

    if (bitmap_or_error.is_error()) {
        dbgln_if(EMOJI_DEBUG, "Generated emoji data has file {}, but could not load image: {}", *emoji_file, bitmap_or_error.error());
        s_emojis.set(*emoji_file, nullptr);
        return nullptr;
    }

    auto bitmap = bitmap_or_error.release_value();
    s_emojis.set(*emoji_file, bitmap);
    return bitmap.ptr();
}

template<typename CodePointIterator>
static Bitmap const* emoji_for_code_point_iterator_impl(CodePointIterator& it)
{
    if (!Unicode::could_be_start_of_emoji_sequence(it))
        return nullptr;

    constexpr size_t max_emoji_code_point_sequence_length = 10;

    Vector<u32, max_emoji_code_point_sequence_length> code_points;

    struct EmojiAndCodePoints {
        Bitmap const* emoji;
        Span<u32> code_points;
        u8 real_codepoint_length;
    };
    Vector<EmojiAndCodePoints, max_emoji_code_point_sequence_length> possible_emojis;

    // Determine all existing emojis for the longest possible ZWJ emoji sequence,
    // or until we run out of code points in the iterator.
    bool last_codepoint_sequence_found = false;
    for (u8 i = 0; i < max_emoji_code_point_sequence_length; ++i) {
        auto code_point = it.peek(i);
        if (!code_point.has_value())
            break;
        // NOTE: The following only applies to emoji presentation, not to other
        // emoji modifiers.
        //
        // For a single emoji core sequence, we assume that emoji presentation
        // is implied, since this function will only be called for characters
        // with default text presentation when either (1) the character is not
        // found in the font, or (2) the character is followed by an explicit
        // emoji presentation selector.
        //
        // For emoji zwj sequences, Serenity chooses to treat minimally-qualified
        // and unqualified emojis the same as fully-qualified emojis (with regards
        // to emoji presentation).
        //
        // From https://unicode.org/reports/tr51/#Emoji_Implementation_Notes:
        // > minimally-qualified or unqualified emoji zwj sequences may be handled
        // > in the same way as their fully-qualified forms; the choice is up to
        // > the implementation.
        //
        // In both cases, whenever an emoji presentation selector (U+FE0F) is found, we
        // just skip it in order to drop fully-qualified emojis down to their
        // minimally-qualified or unqualified forms (with respect to emoji presentation)
        // for doing emoji lookups. This ensures that all forms are treated the same
        // assuming the emoji filenames are named accordingly (with all emoji presentation
        // selector codepoints removed).
        if (code_point.value() == 0xFE0F) {
            // If the last sequence was found, then we can just update
            // its real length.
            if (last_codepoint_sequence_found) {
                possible_emojis.last().real_codepoint_length++;
            }
            // And we can always skip the lookup since the code point sequence
            // will be unchanged since last time.
            continue;
        } else {
            code_points.append(*code_point);
        }
        if (auto const* emoji = Emoji::emoji_for_code_points(code_points)) {
            u8 real_codepoint_length = i + 1;
            possible_emojis.empend(emoji, code_points, real_codepoint_length);
            last_codepoint_sequence_found = true;
        } else {
            last_codepoint_sequence_found = false;
        }
    }

    if (possible_emojis.is_empty())
        return nullptr;

    // If we found one or more matches, return the longest, i.e. last. For example:
    // U+1F3F3 - white flag
    // U+1F3F3 U+200D U+1F308 - rainbow flag (unqualified form)
    auto& [emoji, emoji_code_points, codepoint_length] = possible_emojis.last();

    // Advance the iterator, so it's on the last code point of our found emoji and
    // whoever is iterating will advance to the next new code point.
    for (u8 i = 0; i < codepoint_length - 1; ++i)
        ++it;

    return emoji;
}

Bitmap const* Emoji::emoji_for_code_point_iterator(Utf8CodePointIterator& it)
{
    return emoji_for_code_point_iterator_impl(it);
}

Bitmap const* Emoji::emoji_for_code_point_iterator(Utf32CodePointIterator& it)
{
    return emoji_for_code_point_iterator_impl(it);
}

}
