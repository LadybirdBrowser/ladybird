/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class InlinePaintable;

InlinePaintable const* nearest_self_painting_inline_box(Layout::Node const&);

class PaintableWithLines : public Paintable {
public:
    static NonnullRefPtr<PaintableWithLines> create(Layout::BlockContainer const&);
    virtual ~PaintableWithLines() override;

    struct CaretPaint {
        CSSPixelRect rect;
        Color color;
    };
    Optional<CaretPaint> resolve_caret_paint(InlinePaintable const* owner) const;

    // Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
    // rendered by a <br> child or in an empty editable element.
    CSSPixelRect caret_rect_for_child_offset(size_t offset) const;

protected:
    PaintableWithLines(Layout::BlockContainer const&);
};

}
