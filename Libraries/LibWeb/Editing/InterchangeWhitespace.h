/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// INTEROP: Blink and WebKit rebalance collapsible whitespace into ordinary text and protected spaces when moving rich
//          content between documents. Keeping that policy as representation-neutral tokens lets clipboard
//          serialization wrap protected spaces in spans while replacement can use the same policy for unprotected
//          external HTML without coupling either operation to the other's DOM shape.
enum class InterchangeWhitespaceTokenType : u8 {
    Text,
    ProtectedSpace,
};

struct InterchangeWhitespaceToken {
    InterchangeWhitespaceTokenType type;
    Utf16String text;
};

Vector<InterchangeWhitespaceToken> rebalance_whitespace_for_interchange(Utf16View);
bool is_ascii_whitespace_text(DOM::Node const&);

}
