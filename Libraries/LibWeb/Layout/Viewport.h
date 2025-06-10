/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class Viewport final : public BlockContainer {
    GC_CELL(Viewport, BlockContainer);
    GC_DECLARE_ALLOCATOR(Viewport);

public:
    explicit Viewport(DOM::Document&, GC::Ref<CSS::ComputedProperties>);
    virtual ~Viewport() override;

    struct TextPosition {
        GC::Ref<DOM::Text> dom_node;
        size_t start_offset { 0 };
    };
    struct TextBlock {
        AK::Utf16ConversionResult text;
        Vector<TextPosition> positions;
    };
    Vector<TextBlock> const& text_blocks();
    void invalidate_text_blocks_cache() { m_text_blocks.clear(); }

    const DOM::Document& dom_node() const { return static_cast<const DOM::Document&>(*Node::dom_node()); }

    virtual void visit_edges(Visitor&) override;

private:
    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

    void update_text_blocks();

    virtual bool is_viewport() const override { return true; }

    Optional<Vector<TextBlock>> m_text_blocks;
};

template<>
inline bool Node::fast_is<Viewport>() const { return is_viewport(); }

}
