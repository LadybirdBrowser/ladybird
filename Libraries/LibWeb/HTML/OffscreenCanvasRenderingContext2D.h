/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/AbstractCanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvas.h>

namespace Web::HTML {

class OffscreenCanvasRenderingContext2D : public Bindings::PlatformObject
    , public AbstractCanvasRenderingContext2D<OffscreenCanvasRenderingContext2D, OffscreenCanvas> {

    WEB_PLATFORM_OBJECT(OffscreenCanvasRenderingContext2D, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

public:
    [[nodiscard]] static JS::ThrowCompletionOr<GC::Ref<OffscreenCanvasRenderingContext2D>> create(JS::Realm&, OffscreenCanvas&, JS::Value);
    virtual ~OffscreenCanvasRenderingContext2D() override;

    [[nodiscard]] Gfx::Painter* painter() override;

    void allocate_painting_surface_if_needed() override;

    virtual void set_shadow_color(String) override;
    virtual void set_filter(String filter) override;

    GC::Ref<OffscreenCanvas> canvas() { return m_element; }
    OffscreenCanvas& canvas_element() { return *m_element; }
    OffscreenCanvas const& canvas_element() const { return *m_element; }
    virtual JS::Realm& realm() const override
    {
        return PlatformObject::realm();
    }

private:
    virtual void did_draw(Gfx::FloatRect const&) override;

    explicit OffscreenCanvasRenderingContext2D(JS::Realm&, OffscreenCanvas&, CanvasRenderingContext2DSettings);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Gfx::ShareableBitmap m_bitmap;
};

}
