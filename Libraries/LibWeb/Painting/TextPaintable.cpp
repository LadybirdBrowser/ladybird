/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Painting {

NonnullRefPtr<TextPaintable> TextPaintable::create(Layout::TextNode const& layout_node)
{
    return adopt_ref(*new TextPaintable(layout_node));
}

TextPaintable::TextPaintable(Layout::TextNode const& layout_node)
    : Paintable(layout_node)
{
}

void TextPaintable::paint_inspector_overlay_internal(DisplayListRecordingContext& context) const
{
    auto parent_paintable = parent();
    if (auto const* paintable_with_lines = as_if<PaintableWithLines>(parent_paintable.ptr())) {
        for (auto const& fragment : paintable_with_lines->fragments()) {
            if (&fragment.paintable() == this) {
                PaintableWithLines::paint_text_fragment_debug_highlight(context, fragment);
            }
        }
    }
}

}
