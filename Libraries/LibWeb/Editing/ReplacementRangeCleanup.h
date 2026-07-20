/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Range.h>

namespace Web::Editing {

class InsertedContent;

// Finalize the mutation-tracked inserted range after its paragraphs have been integrated. Whitespace normalization
// and text-node coalescing are deliberately ordered here so every insertion topology repairs the same logical seams.
DOM::BoundaryPoint finalize_inserted_content(InsertedContent&, DOM::BoundaryPoint end);

}
