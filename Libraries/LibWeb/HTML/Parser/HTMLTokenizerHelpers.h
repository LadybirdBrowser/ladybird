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

// Swift-friendly wrapper for TextCodec::Decoder::to_utf8
using OptionalString = std::optional<String>;
OptionalString decode_to_utf8(StringView text, StringView encoding);

}
