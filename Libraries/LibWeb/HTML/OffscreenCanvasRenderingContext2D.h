/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>
#include <LibWeb/HTML/Canvas/CanvasTextDrawingStyles.h>

namespace Web::HTML {

class OffscreenCanvasRenderingContext2D
    : public Canvas2DContextBase
    , public CanvasTextDrawingStyles<OffscreenCanvas> {

    WEB_WRAPPABLE(OffscreenCanvasRenderingContext2D, Canvas2DContextBase);
    GC_DECLARE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

public:
    [[nodiscard]] static GC::Ref<OffscreenCanvasRenderingContext2D> create(OffscreenCanvas&, HTML::CanvasRenderingContext2DSettings);
    virtual ~OffscreenCanvasRenderingContext2D() override;

    GC::Ref<OffscreenCanvas> canvas();

    void replace_bitmap_with_cleared_bitmap();

protected:
    virtual CanvasHost& canvas_host() const override;

private:
    OffscreenCanvasRenderingContext2D(JS::Realm&, OffscreenCanvas&, HTML::CanvasRenderingContext2DSettings);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<OffscreenCanvas> m_canvas;
};

}
