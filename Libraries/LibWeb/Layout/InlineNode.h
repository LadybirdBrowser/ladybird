/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class InlineNode final : public NodeWithStyleAndBoxModelMetrics {
    GC_CELL(InlineNode, NodeWithStyleAndBoxModelMetrics);
    GC_DECLARE_ALLOCATOR(InlineNode);

public:
    InlineNode(DOM::Document&, DOM::Element*, CSS::StyleProperties);
    virtual ~InlineNode() override;

    GC::Ptr<Painting::PaintableWithLines> create_paintable_for_line_with_index(size_t line_index) const;
};

}
