/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>

namespace Web::DOM {

class Range;

}

namespace Web::Editing {

// Serialize the rendered selection for rich clipboard interchange. Unlike DOM fragment serialization, this preserves
// visual paragraph boundaries, collapsible whitespace, list structure, and formatting inherited from outside a range.
Utf16String serialize_styled_markup_for_clipboard(DOM::Range&);

}
