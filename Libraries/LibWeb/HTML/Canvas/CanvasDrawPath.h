/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibWeb/HTML/Path2D.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasdrawpath
class CanvasDrawPath {
public:
    virtual ~CanvasDrawPath() = default;

    virtual void begin_path() = 0;

    virtual void fill(StringView fill_rule) = 0;
    virtual void fill(Path2D& path, StringView fill_rule) = 0;

    virtual void stroke() = 0;
    virtual void stroke(Path2D const& path) = 0;

    virtual void clip(StringView fill_rule) = 0;
    virtual void clip(Path2D& path, StringView fill_rule) = 0;

    virtual bool is_point_in_path(double x, double y, StringView fill_rule) = 0;
    virtual bool is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule) = 0;

protected:
    CanvasDrawPath() = default;
};

}
