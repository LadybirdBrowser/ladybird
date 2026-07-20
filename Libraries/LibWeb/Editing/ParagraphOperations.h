/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Range.h>

namespace Web::Editing {

// Expose a boundary directly in its containing block, cloning inline ancestors so content on either side retains its
// wrappers. All mutations use editing commands and therefore participate in undo and redo.
DOM::BoundaryPoint split_inline_ancestors_at_boundary(DOM::BoundaryPoint, GC::Ref<DOM::Node> containing_block);

// Split the containing block at a paragraph position and return the block holding the paragraph suffix.
GC::Ref<DOM::Node> split_containing_block_at_boundary(DOM::BoundaryPoint, GC::Ref<DOM::Node> containing_block);

}
