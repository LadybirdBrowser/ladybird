/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <optional>

namespace Web::HTML {

// Swift-friendly wrapper for TextCodec::Decoder::to_utf8
using OptionalString = std::optional<String>;
OptionalString decode_to_utf8(StringView text, StringView encoding);

// Swift-friendly wrapper for HTML::code_points_from_entity
using OptionalEntityMatch = std::optional<EntityMatch>;
OptionalEntityMatch match_entity_for_named_character_reference(StringView entity);

}
