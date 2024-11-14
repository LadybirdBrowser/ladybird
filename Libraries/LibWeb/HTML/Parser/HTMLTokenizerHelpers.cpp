/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTextCodec/Decoder.h>
#include <LibWeb/HTML/Parser/HTMLTokenizerHelpers.h>

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
    auto entity_match = code_points_from_entity(entity);
    if (entity_match.has_value())
        return entity_match.release_value();
    return std::nullopt;
}

}
