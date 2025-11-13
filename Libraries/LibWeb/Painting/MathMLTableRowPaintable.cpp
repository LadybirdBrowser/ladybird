/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLTableRowPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLTableRowPaintable);

GC::Ref<MathMLTableRowPaintable> MathMLTableRowPaintable::create(Layout::MathMLTableRowBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLTableRowPaintable>(layout_box);
}

MathMLTableRowPaintable::MathMLTableRowPaintable(Layout::MathMLTableRowBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLTableRowBox const& MathMLTableRowPaintable::layout_box() const
{
    return static_cast<Layout::MathMLTableRowBox const&>(layout_node());
}

void MathMLTableRowPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // Paint the background and borders like a normal box
    PaintableBox::paint(context, phase);
}

}
