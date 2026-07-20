/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Range.h>

namespace Web::Editing {

struct InlineInsertionBoundary {
    DOM::BoundaryPoint boundary;
    bool split_destination_style { false };
};

InlineInsertionBoundary prepare_inline_insertion_boundary(DOM::BoundaryPoint, DOM::Node& containing_block);

}
