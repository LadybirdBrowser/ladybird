/*
 * Copyright (c) 2024, İbrahim UYSAL <uysalibov@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasshadowstyles
class CanvasShadowStyles {
public:
    ~CanvasShadowStyles() = default;

    virtual float shadow_offset_x() const = 0;
    virtual void set_shadow_offset_x(float offsetX) = 0;

    virtual float shadow_offset_y() const = 0;
    virtual void set_shadow_offset_y(float offsetY) = 0;

    virtual float shadow_blur() const = 0;
    virtual void set_shadow_blur(float offsetY) = 0;

    virtual String shadow_color() const = 0;
    virtual void set_shadow_color(String color) = 0;

protected:
    CanvasShadowStyles() = default;
};

}
