/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class ListItemMarkerBox final : public Box {
    GC_CELL(ListItemMarkerBox, Box);
    GC_DECLARE_ALLOCATOR(ListItemMarkerBox);

public:
    explicit ListItemMarkerBox(DOM::Document&, CSS::ListStyleType, CSS::ListStylePosition, GC::Ref<DOM::Element>, GC::Ref<CSS::ComputedProperties>);
    virtual ~ListItemMarkerBox() override;

    Optional<String> text() const;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

    CSS::ListStyleType const& list_style_type() const { return m_list_style_type; }
    CSS::ListStylePosition list_style_position() const { return m_list_style_position; }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_list_item_marker_box() const final { return true; }
    virtual bool can_have_children() const override { return false; }

    CSS::ListStyleType m_list_style_type { CSS::CounterStyleNameKeyword::None };
    CSS::ListStylePosition m_list_style_position { CSS::ListStylePosition::Outside };
    GC::Ref<DOM::Element> m_list_item_element;
};

template<>
inline bool Node::fast_is<ListItemMarkerBox>() const { return is_list_item_marker_box(); }

}
