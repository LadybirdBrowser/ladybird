/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class StyleInvalidator : public GC::Cell {
    GC_CELL(StyleInvalidator, GC::Cell);
    GC_DECLARE_ALLOCATOR(StyleInvalidator);

public:
    void perform_pending_style_invalidations(Node& node, bool invalidate_entire_subtree);
};

}
