/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>

namespace Web::HTML {

// Returns true for an empty type, or for a MIME type that our image decoder
// is known to support. The list is currently hard-coded; eventually it should
// be derived from the registered image decoders.
bool is_supported_image_type(StringView type);
bool is_supported_image_type(Utf16View type);

}
