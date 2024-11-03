/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

class CanvasGradient final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CanvasGradient, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(CanvasGradient);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CanvasGradient>> create_radial(JS::Realm&, double x0, double y0, double r0, double x1, double y1, double r1);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CanvasGradient>> create_linear(JS::Realm&, double x0, double y0, double x1, double y1);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CanvasGradient>> create_conic(JS::Realm&, double start_angle, double x, double y);

    WebIDL::ExceptionOr<void> add_color_stop(double offset, StringView color);

    ~CanvasGradient();

    NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style() { return m_gradient; }

private:
    CanvasGradient(JS::Realm&, Gfx::GradientPaintStyle& gradient);

    virtual void initialize(JS::Realm&) override;

    NonnullRefPtr<Gfx::GradientPaintStyle> m_gradient;
};

}
