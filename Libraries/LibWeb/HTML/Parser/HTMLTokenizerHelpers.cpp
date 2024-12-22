/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTextCodec/Decoder.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <LibWeb/HTML/Parser/HTMLTokenizerHelpers.h>
#include <LibWeb/HTML/Parser/NamedCharacterReferences.h>

namespace Web::HTML {

OptionalString decode_to_utf8(StringView text, StringView encoding)
{
    auto decoder = TextCodec::decoder_for(encoding);
    if (!decoder.has_value())
        return std::nullopt;
    auto decoded_or_error = decoder.value().to_utf8(text);
    if (decoded_or_error.is_error())
        return std::nullopt;
    return decoded_or_error.release_value();
}

OptionalEntityMatch match_entity_for_named_character_reference(StringView entity)
{
    NamedCharacterReferenceMatcher matcher;
    int consumed_length = 0;
    for (auto c : entity) {
        if (!matcher.try_consume_ascii_char(c))
            break;
        consumed_length++;
    }

    auto codepoints = matcher.code_points();
    if (codepoints.has_value()) {
        EntityMatch match;
        auto matched_length = consumed_length - matcher.overconsumed_code_points();
        auto matched_string_view = entity.substring_view(0, matched_length);
        auto second_codepoint = named_character_reference_second_codepoint_value(codepoints.value().second);
        if (second_codepoint.has_value()) {
            match = { { codepoints.value().first, second_codepoint.release_value() }, matched_string_view };
        } else {
            match = { { codepoints.value().first }, matched_string_view };
        }
        return match;
    }
    return std::nullopt;
}

}
