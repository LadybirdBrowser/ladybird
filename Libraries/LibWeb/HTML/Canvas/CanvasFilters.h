/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasfilters
class CanvasFilters : virtual public AbstractCanvasRenderingContext2DBase {
public:
    ~CanvasFilters() = default;

    String filter() const;
    virtual void set_filter(String filter) = 0;

protected:
    CanvasFilters() = default;
};

}
