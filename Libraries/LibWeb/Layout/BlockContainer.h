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
    BlockContainer(DOM::Document&, GC::Ptr<DOM::Node>, CSS::LayoutStyle, RustFFI::NodeKind = RustFFI::NodeKind::BlockContainer);
    virtual ~BlockContainer() override;

private:
    virtual bool is_block_container() const final { return true; }
};

template<>
inline bool Node::fast_is<BlockContainer>() const { return is_block_container(); }

}
