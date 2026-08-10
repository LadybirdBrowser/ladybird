/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class ListItemBox final : public BlockContainer {
    LAYOUT_NODE(ListItemBox, BlockContainer);

public:
    ListItemBox(DOM::Document&, GC::Ptr<DOM::Element>, NonnullRefPtr<CSS::ComputedValues const>);
    virtual ~ListItemBox() override;

    DOM::Element& dom_node() { return static_cast<DOM::Element&>(*BlockContainer::dom_node()); }
    DOM::Element const& dom_node() const { return static_cast<DOM::Element const&>(*BlockContainer::dom_node()); }

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_list_item_box() const override { return true; }
};

template<>
inline bool Node::fast_is<ListItemBox>() const { return is_list_item_box(); }

}
