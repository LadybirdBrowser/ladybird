/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvascompositing
class CanvasCompositing {
public:
    virtual ~CanvasCompositing() = default;

    virtual float global_alpha() const = 0;
    virtual void set_global_alpha(float) = 0;

    virtual Utf16String global_composite_operation() const = 0;
    virtual void set_global_composite_operation(Utf16View) = 0;

protected:
    CanvasCompositing() = default;
};

}
