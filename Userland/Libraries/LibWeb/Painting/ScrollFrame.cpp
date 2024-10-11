/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

ScrollFrame::ScrollFrame(PaintableBox const& paintable_box, size_t id, RefPtr<ScrollFrame const> parent)
    : m_paintable_box(paintable_box)
    , m_id(id)
    , m_parent(move(parent))
{
}

}
