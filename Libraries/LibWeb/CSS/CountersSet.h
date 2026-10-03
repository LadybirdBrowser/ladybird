/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// "UAs may have implementation-specific limits on the maximum or minimum value of a counter.
// If a counter reset, set, or increment would push the value outside of that range, the value
// must be clamped to that range." - https://drafts.csswg.org/css-lists-3/#auto-numbering
using CounterValue = i32;

// NB: The CSS counters sets live in the layout node arena, which resolves them during the layout tree build.
bool innermost_list_item_counter_is_own_forward_counter(Layout::BegunRead const&, DOM::Element const&);

Utf16FlyString const& list_item_counter_name();

}
