/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLMultiscriptsPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLMultiscriptsPaintable);

GC::Ref<MathMLMultiscriptsPaintable> MathMLMultiscriptsPaintable::create(Layout::MathMLMultiscriptsBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLMultiscriptsPaintable>(layout_box);
}

MathMLMultiscriptsPaintable::MathMLMultiscriptsPaintable(Layout::MathMLMultiscriptsBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLMultiscriptsBox const& MathMLMultiscriptsPaintable::layout_box() const
{
    return static_cast<Layout::MathMLMultiscriptsBox const&>(layout_node());
}

void MathMLMultiscriptsPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // Paint the background and borders like a normal box
    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    // The actual positioning of subscripts and superscripts is handled by layout
    // This paintable just ensures proper rendering of the children
}

}
