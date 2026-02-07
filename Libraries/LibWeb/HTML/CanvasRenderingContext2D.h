/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibGfx/Painter.h>
#include <LibWeb/HTML/AbstractCanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class CanvasRenderingContext2D
    : public Bindings::PlatformObject
    , public AbstractCanvasRenderingContext2D<CanvasRenderingContext2D, HTMLCanvasElement> {

    WEB_PLATFORM_OBJECT(CanvasRenderingContext2D, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CanvasRenderingContext2D);

public:
    static JS::ThrowCompletionOr<GC::Ref<CanvasRenderingContext2D>> create(JS::Realm&, HTMLCanvasElement&, JS::Value options);

    virtual void set_shadow_color(String color) override;
    virtual void set_filter(String filter) override;

    [[nodiscard]] virtual Gfx::Painter* painter() override;

    virtual void allocate_painting_surface_if_needed() override;

    CanvasRenderingContext2D(JS::Realm&, HTMLCanvasElement&, CanvasRenderingContext2DSettings);

    virtual JS::Realm& realm() const override
    {
        return PlatformObject::realm();
    }

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void did_draw(Gfx::FloatRect const&) override;

    virtual bool is_canvas_rendering_context_2d() const final { return true; }
};

}

namespace JS {

template<>
inline bool Object::fast_is<Web::HTML::CanvasRenderingContext2D>() const { return is_canvas_rendering_context_2d(); }

}
