/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

ScrollFrame::ScrollFrame(VisualContextIndex node_index, Paintable const& paintable_box, bool sticky, ScrollStateSlot parent_slot)
    : m_paintable_box(paintable_box)
    , m_sticky(sticky)
    , m_node_index(node_index)
    , m_parent_slot(parent_slot)
{
}

RefPtr<Paintable const> ScrollFrame::paintable_box_if_alive() const
{
    return m_paintable_box.strong_ref();
}

}
