/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

// https://www.w3.org/TR/css-display/#block-container
class BlockContainer : public Box {
public:
    BlockContainer(DOM::Document& document, GC::Ptr<DOM::Node> node, CSS::LayoutStyle style, RustFFI::NodeKind kind = RustFFI::NodeKind::BlockContainer)
        : Box(document, node, move(style), kind)
    {
    }
    BlockContainer(DOM::Document& document, BindToPreparedArenaSlot bind, RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
        : Box(document, bind, slot, kind)
    {
    }
    virtual ~BlockContainer() override = default;

private:
    virtual bool is_block_container() const final { return true; }
};

template<>
inline bool Node::fast_is<BlockContainer>() const { return is_block_container(); }

}
