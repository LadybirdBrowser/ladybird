/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLTablePaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLTablePaintable);

GC::Ref<MathMLTablePaintable> MathMLTablePaintable::create(Layout::MathMLTableBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLTablePaintable>(layout_box);
}

MathMLTablePaintable::MathMLTablePaintable(Layout::MathMLTableBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLTableBox const& MathMLTablePaintable::layout_box() const
{
    return static_cast<Layout::MathMLTableBox const&>(layout_node());
}

void MathMLTablePaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // Paint the background and borders like a normal box
    PaintableBox::paint(context, phase);
}

}
