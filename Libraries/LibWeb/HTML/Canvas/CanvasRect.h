/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasrect
class CanvasRect {
public:
    virtual ~CanvasRect() = default;

    virtual void fill_rect(float x, float y, float width, float height) = 0;
    virtual void stroke_rect(float x, float y, float width, float height) = 0;
    virtual void clear_rect(float x, float y, float width, float height) = 0;

protected:
    CanvasRect() = default;
};

}
