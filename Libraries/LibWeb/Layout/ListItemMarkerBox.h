/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class ListItemMarkerBox final : public BlockContainer {
    LAYOUT_NODE(ListItemMarkerBox, BlockContainer);

public:
    static bool counter_style_is_rendered_with_custom_image(RefPtr<CSS::CounterStyle const> const& counter_style);

    explicit ListItemMarkerBox(DOM::Document&, CSS::ListStyleType, CSS::ListStylePosition, NonnullRefPtr<CSS::ComputedValues const>);
    virtual ~ListItemMarkerBox() override;

    bool is_symbolic() const;
    bool has_symbolic_counter_style() const;

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

    CSS::ListStyleType const& list_style_type() const { return m_list_style_type; }
    CSS::ListStylePosition list_style_position() const { return m_list_style_position; }

    CSSPixels relative_size() const;

private:
    virtual bool is_list_item_marker_box() const final { return true; }

    CSS::ListStyleType m_list_style_type;
    CSS::ListStylePosition m_list_style_position { CSS::ListStylePosition::Outside };
};

template<>
inline bool Node::fast_is<ListItemMarkerBox>() const { return is_list_item_marker_box(); }

}
