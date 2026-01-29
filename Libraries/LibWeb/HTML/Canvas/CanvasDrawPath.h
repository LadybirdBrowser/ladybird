/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/Path2D.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasdrawpath
class CanvasDrawPath : virtual public AbstractCanvasRenderingContext2DBase {
public:
    void begin_path();

    void fill(StringView fill_rule);
    void fill(Path2D& path, StringView fill_rule);

    void stroke();
    void stroke(Path2D const& path);

    void clip(StringView fill_rule);
    void clip(Path2D& path, StringView fill_rule);

    bool is_point_in_path(double x, double y, StringView fill_rule);
    bool is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule);

protected:
    CanvasDrawPath() = default;
};

}
