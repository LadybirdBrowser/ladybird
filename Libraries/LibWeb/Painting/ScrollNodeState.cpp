/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/ScrollNodeState.h>

namespace Web::Painting {

ScrollNodeState::ScrollNodeState(VisualContextIndex node_index, Paintable const& paintable_box, bool sticky, ScrollStateSlot parent_slot)
    : m_paintable_box(paintable_box)
    , m_sticky(sticky)
    , m_node_index(node_index)
    , m_parent_slot(parent_slot)
{
}

RefPtr<Paintable const> ScrollNodeState::paintable_box_if_alive() const
{
    return m_paintable_box.strong_ref();
}

}
