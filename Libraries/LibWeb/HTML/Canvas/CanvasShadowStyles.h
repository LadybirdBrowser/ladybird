/*
 * Copyright (c) 2024, İbrahim UYSAL <uysalibov@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasshadowstyles
template<typename IncludingClass>
class CanvasShadowStyles : public virtual AbstractCanvasRenderingContext2DBase {
public:
    ~CanvasShadowStyles() = default;

    virtual float shadow_offset_x() const;
    virtual void set_shadow_offset_x(float offsetX);

    virtual float shadow_offset_y() const;
    virtual void set_shadow_offset_y(float offsetY);

    virtual float shadow_blur() const;
    virtual void set_shadow_blur(float offsetY);

    virtual String shadow_color() const;
    virtual void set_shadow_color(String color) = 0;

protected:
    CanvasShadowStyles() = default;

private:
    DrawingState& my_drawing_state() { return static_cast<IncludingClass&>(*this).drawing_state(); }
    DrawingState const& my_drawing_state() const { return static_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
