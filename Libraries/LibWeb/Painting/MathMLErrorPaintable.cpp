/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLErrorPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLErrorPaintable);

GC::Ref<MathMLErrorPaintable> MathMLErrorPaintable::create(Layout::MathMLErrorBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLErrorPaintable>(layout_box);
}

MathMLErrorPaintable::MathMLErrorPaintable(Layout::MathMLErrorBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLErrorBox const& MathMLErrorPaintable::layout_box() const
{
    return static_cast<Layout::MathMLErrorBox const&>(layout_node());
}

void MathMLErrorPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // First paint the background and borders like a normal box
    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    // Draw a border around error messages to indicate errors
    auto content_rect = absolute_rect();
    context.display_list_recorder().draw_rect(
        content_rect.to_type<int>(),
        Gfx::Color::Red,
        false);
}

}
