/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16View.h>
#include <LibWeb/HTML/TextMetrics.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvastext
class CanvasText {
public:
    virtual ~CanvasText() = default;

    virtual void fill_text(Utf16View, float x, float y, Optional<double> max_width) = 0;
    virtual void stroke_text(Utf16View, float x, float y, Optional<double> max_width) = 0;
    virtual GC::Ref<TextMetrics> measure_text(Utf16View) = 0;

protected:
    CanvasText() = default;
};

}
