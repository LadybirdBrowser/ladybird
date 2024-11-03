/*
 * Copyright (c) 2024, İbrahim UYSAL <uysalibov@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasshadowstyles
template<typename IncludingClass>
class CanvasShadowStyles {
public:
    ~CanvasShadowStyles() = default;

    virtual float shadow_offset_x() const = 0;
    virtual void set_shadow_offset_x(float offsetX) = 0;

    virtual float shadow_offset_y() const = 0;
    virtual void set_shadow_offset_y(float offsetY) = 0;

    virtual String shadow_color() const = 0;
    virtual void set_shadow_color(String color) = 0;

protected:
    CanvasShadowStyles() = default;

private:
    CanvasState::DrawingState& my_drawing_state() { return reinterpret_cast<IncludingClass&>(*this).drawing_state(); }
    CanvasState::DrawingState const& my_drawing_state() const { return reinterpret_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
