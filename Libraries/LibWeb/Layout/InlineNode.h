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
    InlineNode(DOM::Document&, DOM::Element*, NonnullRefPtr<CSS::ComputedValues const>);
    virtual ~InlineNode() override;

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_inline_node() const override { return true; }
};

template<>
inline bool Node::fast_is<InlineNode>() const { return is_inline_node(); }

}
