/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLUnderOverPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLUnderOverPaintable);

GC::Ref<MathMLUnderOverPaintable> MathMLUnderOverPaintable::create(Layout::MathMLUnderOverBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLUnderOverPaintable>(layout_box);
}

MathMLUnderOverPaintable::MathMLUnderOverPaintable(Layout::MathMLUnderOverBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLUnderOverBox const& MathMLUnderOverPaintable::layout_box() const
{
    return static_cast<Layout::MathMLUnderOverBox const&>(layout_node());
}

void MathMLUnderOverPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // First paint the background and borders like a normal box
    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    // Underover elements rely on the layout system for positioning
    // The children (base, underscript, overscript) are painted by PaintableBox::paint above
}

}
