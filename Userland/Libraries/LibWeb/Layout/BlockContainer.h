/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/AvailableSpace.h>
#include <LibWeb/Layout/LineBox.h>
#include <LibWeb/Layout/Node.h>

class LayoutState;

namespace Web::Layout {

// https://www.w3.org/TR/css-display/#block-container
class BlockContainer : public Box {
    JS_CELL(BlockContainer, Box);

public:
    BlockContainer(DOM::Document&, DOM::Node*, NonnullRefPtr<CSS::StyleProperties>);
    BlockContainer(DOM::Document&, DOM::Node*, NonnullOwnPtr<CSS::ComputedValues>);
    virtual ~BlockContainer() override;

    Painting::PaintableWithLines const* paintable_with_lines() const;

    void store_layout_inside_run_info(LayoutState*, AvailableSpace const);

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

    // Save info for layout invalidation input box fast path
    AvailableSpace m_run_available_space = AvailableSpace(AvailableSize::make_definite(0), AvailableSize::make_definite(0));

private:
    virtual bool is_block_container() const final { return true; }
};

template<>
inline bool Node::fast_is<BlockContainer>() const { return is_block_container(); }

}
