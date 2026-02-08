/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

ScrollFrame::ScrollFrame(PaintableBox const& paintable_box, size_t id, bool sticky, RefPtr<ScrollFrame const> parent)
    : m_paintable_box(paintable_box)
    , m_id(id)
    , m_sticky(sticky)
    , m_parent(move(parent))
{
}

RefPtr<ScrollFrame const> ScrollFrame::nearest_scrolling_ancestor() const
{
    for (auto ancestor = m_parent; ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->is_sticky())
            return ancestor;
    }
    return nullptr;
}

}
