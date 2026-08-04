/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>
#include <LibWeb/HTML/Canvas/CanvasTextDrawingStyles.h>

namespace Web::HTML {

class CanvasRenderingContext2D
    : public Canvas2DContextBase
    , public CanvasTextDrawingStyles<HTMLCanvasElement> {

    WEB_WRAPPABLE(CanvasRenderingContext2D, Canvas2DContextBase);
    GC_DECLARE_ALLOCATOR(CanvasRenderingContext2D);

public:
    static GC::Ref<CanvasRenderingContext2D> create(HTMLCanvasElement&, HTML::CanvasRenderingContext2DSettings);

    virtual ~CanvasRenderingContext2D() override;

    GC::Ref<HTMLCanvasElement> canvas_for_binding() const;

protected:
    Variant<GC::Ref<HTMLCanvasElement>, GC::Ref<OffscreenCanvas>> canvas_element() override { return m_element; }
    Variant<GC::Ref<HTMLCanvasElement>, GC::Ref<OffscreenCanvas>> canvas_element() const override { return m_element; }

private:
    CanvasRenderingContext2D(JS::Realm&, HTMLCanvasElement&, HTML::CanvasRenderingContext2DSettings);

    virtual bool is_canvas_rendering_context_2d() const final { return true; }
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    virtual void visit_edges(Cell::Visitor&) override;

    virtual void did_draw_hook() override;
    virtual Page* page_for_compositor() override;
    virtual void backing_storage_created_hook() override;
    virtual DOM::EventTarget& context_event_target() override;
    virtual Gfx::Color resolve_drop_shadow_color(CSS::DropShadowFilterStyleValue const&) const override;

    GC::Ref<HTMLCanvasElement> m_element;
};

}
