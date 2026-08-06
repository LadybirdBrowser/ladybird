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
    explicit ListItemMarkerBox(DOM::Document&, bool is_inside, NonnullRefPtr<CSS::ComputedValues const>);
    virtual ~ListItemMarkerBox() override;

private:
    virtual bool is_list_item_marker_box() const final { return true; }
};

template<>
inline bool Node::fast_is<ListItemMarkerBox>() const { return is_list_item_marker_box(); }

}
