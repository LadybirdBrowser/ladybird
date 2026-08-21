/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

NonnullRefPtr<ViewportPaintable> ViewportPaintable::create(Layout::Viewport const& layout_viewport)
{
    return adopt_ref(*new ViewportPaintable(layout_viewport));
}

ViewportPaintable::ViewportPaintable(Layout::Viewport const& layout_viewport)
    : PaintableWithLines(layout_viewport)
{
    mirror_rust_reset_visual_context_state(document());
}

ViewportPaintable::~ViewportPaintable() = default;

void ViewportPaintable::reset_for_relayout()
{
    PaintableWithLines::reset_for_relayout();
    document().paint_state().viewport_row_was_reset(document());
}

}
