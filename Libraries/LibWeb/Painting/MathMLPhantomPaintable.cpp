/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLPhantomPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLPhantomPaintable);

GC::Ref<MathMLPhantomPaintable> MathMLPhantomPaintable::create(Layout::MathMLPhantomBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLPhantomPaintable>(layout_box);
}

MathMLPhantomPaintable::MathMLPhantomPaintable(Layout::MathMLPhantomBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLPhantomBox const& MathMLPhantomPaintable::layout_box() const
{
    return static_cast<Layout::MathMLPhantomBox const&>(layout_node());
}

void MathMLPhantomPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    // mphantom element renders invisible but preserves space
    // We skip painting the foreground content but still need to paint children invisibly
    // The element takes up space in layout but nothing is visually rendered

    // Don't paint anything - the element is invisible
    // The layout has already been calculated, so space is preserved
    // Children should also be invisible, so we don't call the base implementation
    (void)context;
    (void)phase;
}

}
