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

ScrollFrame::ScrollFrame(PaintableBox const& paintable_box, bool sticky, ScrollFrameIndex parent_index)
    : m_paintable_box(paintable_box)
    , m_sticky(sticky)
    , m_parent_index(parent_index)
{
}

}
