/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasfillstrokestyles
template<typename IncludingClass>
class CanvasFillStrokeStyles {
public:
    ~CanvasFillStrokeStyles() = default;
    using FillOrStrokeStyleVariant = Variant<String, GC::Root<CanvasGradient>, GC::Root<CanvasPattern>>;

    void set_fill_style(FillOrStrokeStyleVariant style);
    FillOrStrokeStyleVariant fill_style() const;

    void set_stroke_style(FillOrStrokeStyleVariant style);
    FillOrStrokeStyleVariant stroke_style() const;

    WebIDL::ExceptionOr<GC::Ref<CanvasGradient>> create_radial_gradient(double x0, double y0, double r0, double x1, double y1, double r1);
    GC::Ref<CanvasGradient> create_linear_gradient(double x0, double y0, double x1, double y1);
    GC::Ref<CanvasGradient> create_conic_gradient(double start_angle, double x, double y);
    WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> create_pattern(CanvasImageSource const& image, StringView repetition);

protected:
    CanvasFillStrokeStyles() = default;

private:
    Variant<HTMLCanvasElement*, OffscreenCanvas*> my_canvas_element();
    CanvasState::DrawingState& my_drawing_state();
    CanvasState::DrawingState const& my_drawing_state() const;
};

}
