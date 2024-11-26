/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class NavigableContainerViewportPaintable final : public PaintableBox {
    GC_CELL(NavigableContainerViewportPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(NavigableContainerViewportPaintable);

public:
    static GC::Ref<NavigableContainerViewportPaintable> create(Layout::NavigableContainerViewport const&);

    virtual void paint(PaintContext&, PaintPhase) const override;

    Layout::NavigableContainerViewport const& layout_box() const;

private:
    NavigableContainerViewportPaintable(Layout::NavigableContainerViewport const&);
};

}
