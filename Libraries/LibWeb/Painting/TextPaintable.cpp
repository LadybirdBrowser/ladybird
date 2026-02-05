/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(TextPaintable);

GC::Ref<TextPaintable> TextPaintable::create(Layout::TextNode const& layout_node)
{
    return layout_node.heap().allocate<TextPaintable>(layout_node);
}

TextPaintable::TextPaintable(Layout::TextNode const& layout_node)
    : Paintable(layout_node)
{
}

void TextPaintable::paint_inspector_overlay_internal(DisplayListRecordingContext& context) const
{
    if (auto const* parent_paintable = as_if<PaintableWithLines>(parent())) {
        for (auto const& fragment : parent_paintable->fragments()) {
            if (&fragment.paintable() == this) {
                PaintableWithLines::paint_text_fragment_debug_highlight(context, fragment);
            }
        }
    }
}

}
