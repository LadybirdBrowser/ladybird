/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/CanvasCommandList.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

GC::Ref<OffscreenCanvasRenderingContext2D> OffscreenCanvasRenderingContext2D::create(OffscreenCanvas& offscreen_canvas, HTML::CanvasRenderingContext2DSettings context_attributes)
{
    auto& realm = offscreen_canvas.relevant_global_object().shape().realm();
    return realm.create<OffscreenCanvasRenderingContext2D>(realm, offscreen_canvas, move(context_attributes));
}

OffscreenCanvasRenderingContext2D::OffscreenCanvasRenderingContext2D(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, HTML::CanvasRenderingContext2DSettings context_attributes)
    : Canvas2DContextBase(realm, offscreen_canvas.bitmap_size_for_canvas(), move(context_attributes))
    , m_canvas(offscreen_canvas)
{
}

OffscreenCanvasRenderingContext2D::~OffscreenCanvasRenderingContext2D() = default;

void OffscreenCanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_canvas);
}

CanvasHost& OffscreenCanvasRenderingContext2D::canvas_host() const
{
    return *m_canvas;
}

GC::Ref<OffscreenCanvas> OffscreenCanvasRenderingContext2D::canvas()
{
    return m_canvas;
}

void OffscreenCanvasRenderingContext2D::replace_bitmap_with_cleared_bitmap()
{
    m_cached_readback = nullptr;
    m_origin_clean = true;
    if (!has_backing_storage())
        return;
    if (auto* canvas_command_list = this->canvas_command_list()) {
        canvas_command_list->append(Gfx::CanvasCommands::ClearCanvas { .color = clear_color() });
        did_draw({ {}, m_size.to_type<float>() });
    }
}

}
