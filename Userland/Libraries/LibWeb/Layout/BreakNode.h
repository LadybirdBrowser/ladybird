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
    JS_CELL(BreakNode, NodeWithStyleAndBoxModelMetrics);
    JS_DECLARE_ALLOCATOR(BreakNode);

public:
    BreakNode(DOM::Document&, HTML::HTMLBRElement&, CSS::StyleProperties);
    virtual ~BreakNode() override;

    const HTML::HTMLBRElement& dom_node() const { return verify_cast<HTML::HTMLBRElement>(*Node::dom_node()); }

private:
    virtual bool is_break_node() const final { return true; }
    virtual bool can_have_children() const override { return false; }
};

template<>
inline bool Node::fast_is<BreakNode>() const { return is_break_node(); }

}
