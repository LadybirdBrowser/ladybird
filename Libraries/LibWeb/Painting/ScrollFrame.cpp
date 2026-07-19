/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

ScrollFrame::ScrollFrame(Paintable const& paintable_box, bool sticky, ScrollFrameIndex parent_index)
    : m_paintable_box(paintable_box)
    , m_sticky(sticky)
    , m_parent_index(parent_index)
{
}

RefPtr<Paintable const> ScrollFrame::paintable_box_if_alive() const
{
    return m_paintable_box.strong_ref();
}

}
