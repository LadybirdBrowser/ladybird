/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/ImageData.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvascompositing
class CanvasCompositing : virtual public AbstractCanvasRenderingContext2DBase {
public:
    float global_alpha() const;
    void set_global_alpha(float);

    String global_composite_operation() const;
    void set_global_composite_operation(String);

protected:
    CanvasCompositing() = default;
};

}
