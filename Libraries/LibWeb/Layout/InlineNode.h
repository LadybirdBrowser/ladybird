/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class InlineNode final : public NodeWithStyleAndBoxModelMetrics {
    LAYOUT_NODE(InlineNode, NodeWithStyleAndBoxModelMetrics);

public:
    InlineNode(DOM::Document&, DOM::Element*, CSS::ComputedProperties const&);
    virtual ~InlineNode() override;

    NonnullRefPtr<Painting::PaintableWithLines> create_paintable_for_line_with_index(size_t line_index) const;

private:
    virtual bool is_inline_node() const override { return true; }
};

template<>
inline bool Node::fast_is<InlineNode>() const { return is_inline_node(); }

}
