/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasrect
class CanvasRect : virtual public AbstractCanvasRenderingContext2DBase {
public:
    void fill_rect(float x, float y, float width, float height);
    void stroke_rect(float x, float y, float width, float height);
    void clear_rect(float x, float y, float width, float height);

protected:
    CanvasRect() = default;
};

}
