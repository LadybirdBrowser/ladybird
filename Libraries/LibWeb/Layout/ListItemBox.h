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
    ListItemBox(DOM::Document&, DOM::Element*, CSS::ComputedProperties const&);
    virtual ~ListItemBox() override;

    DOM::Element& dom_node() { return static_cast<DOM::Element&>(*BlockContainer::dom_node()); }
    DOM::Element const& dom_node() const { return static_cast<DOM::Element const&>(*BlockContainer::dom_node()); }

    ListItemMarkerBox const* marker() const { return m_marker; }
    void set_marker(ListItemMarkerBox*);

private:
    virtual bool is_list_item_box() const override { return true; }

    WeakPtr<ListItemMarkerBox> m_marker;
};

template<>
inline bool Node::fast_is<ListItemBox>() const { return is_list_item_box(); }

}
