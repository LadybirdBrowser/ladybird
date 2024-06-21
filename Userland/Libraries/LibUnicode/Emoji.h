/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace Unicode {

Optional<StringView> emoji_image_for_code_points(ReadonlySpan<u32> code_points);

enum class SequenceType {
    Any,
    EmojiPresentation,
};

bool could_be_start_of_emoji_sequence(Utf8CodePointIterator const&, SequenceType = SequenceType::Any);
bool could_be_start_of_emoji_sequence(Utf32CodePointIterator const&, SequenceType = SequenceType::Any);

}
