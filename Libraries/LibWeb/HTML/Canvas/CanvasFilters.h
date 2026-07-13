/*
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasfilters
class CanvasFilters {
public:
    ~CanvasFilters() = default;

    virtual Utf16String filter() const = 0;
    virtual void set_filter(Utf16View filter) = 0;

protected:
    CanvasFilters() = default;
};

}
