/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/ImageData.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvascompositing
class CanvasCompositing {
public:
    virtual ~CanvasCompositing() = default;

    virtual float global_alpha() const = 0;
    virtual void set_global_alpha(float) = 0;

protected:
    CanvasCompositing() = default;
};

}
