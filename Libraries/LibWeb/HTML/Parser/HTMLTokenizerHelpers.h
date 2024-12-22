/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <optional>

namespace Web::HTML {

// FIXME: This is a temporary stop-gap solution, and it should be removed once the C++
//        NamedCharacterReference state implementation is implemented in Swift.
struct EntityMatch {
    Vector<u32, 2> code_points;
    StringView entity;
};

// Swift-friendly wrapper for TextCodec::Decoder::to_utf8
using OptionalString = std::optional<String>;
OptionalString decode_to_utf8(StringView text, StringView encoding);

// Swift-friendly wrapper for HTML::code_points_from_entity
// FIXME: This is a temporary stop-gap solution, and it should be removed once the C++
//        NamedCharacterReference state implementation is implemented in Swift.
using OptionalEntityMatch = std::optional<EntityMatch>;
OptionalEntityMatch match_entity_for_named_character_reference(StringView entity);

}
