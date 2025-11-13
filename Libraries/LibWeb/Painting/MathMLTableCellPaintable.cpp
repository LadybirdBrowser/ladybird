/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLTableCellPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLTableCellPaintable);

GC::Ref<MathMLTableCellPaintable> MathMLTableCellPaintable::create(Layout::MathMLTableCellBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLTableCellPaintable>(layout_box);
}

MathMLTableCellPaintable::MathMLTableCellPaintable(Layout::MathMLTableCellBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLTableCellBox const& MathMLTableCellPaintable::layout_box() const
{
    return static_cast<Layout::MathMLTableCellBox const&>(layout_node());
}

void MathMLTableCellPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // Paint the background and borders like a normal box
    PaintableBox::paint(context, phase);
}

}
