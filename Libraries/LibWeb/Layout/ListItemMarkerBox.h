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
    explicit ListItemMarkerBox(DOM::Document&, CSS::ListStyleType, CSS::ListStylePosition, size_t index, GC::Ref<CSS::ComputedProperties>);
    virtual ~ListItemMarkerBox() override;

    Optional<String> const& text() const { return m_text; }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

    CSS::ListStyleType list_style_type() const { return m_list_style_type; }
    CSS::ListStylePosition list_style_position() const { return m_list_style_position; }

private:
    virtual bool is_list_item_marker_box() const final { return true; }
    virtual bool can_have_children() const override { return false; }

    CSS::ListStyleType m_list_style_type { CSS::ListStyleType::None };
    CSS::ListStylePosition m_list_style_position { CSS::ListStylePosition::Outside };
    size_t m_index;

    Optional<String> m_text {};
};

template<>
inline bool Node::fast_is<ListItemMarkerBox>() const { return is_list_item_marker_box(); }

}
