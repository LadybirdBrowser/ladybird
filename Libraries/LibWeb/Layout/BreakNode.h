/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

class BreakNode final : public NodeWithStyleAndBoxModelMetrics {
    GC_CELL(BreakNode, NodeWithStyleAndBoxModelMetrics);
    GC_DECLARE_ALLOCATOR(BreakNode);

public:
    BreakNode(DOM::Document&, HTML::HTMLBRElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~BreakNode() override;

    const HTML::HTMLBRElement& dom_node() const { return as<HTML::HTMLBRElement>(*Node::dom_node()); }

private:
    virtual bool is_break_node() const final { return true; }
    virtual bool can_have_children() const override { return false; }
};

template<>
inline bool Node::fast_is<BreakNode>() const { return is_break_node(); }

}
