/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>

namespace Web::CSS::WhiteSpaceProcessing {

// https://drafts.csswg.org/css-text-4/#white-space-phase-1
// Step 1: Any sequence of collapsible spaces and tabs immediately preceding or following a segment break is removed.
Utf16String remove_collapsible_spaces_and_tabs_around_segment_breaks(Utf16String const&);

// Step 2a: First, any collapsible segment break immediately following another collapsible segment break is removed.
Utf16String collapse_consecutive_segment_breaks(Utf16String const&);

// Step 2b: Then any remaining segment break is either transformed into a space or removed depending on context.
Utf16String transform_segment_breaks_for_collapse(Utf16String const&);

}
