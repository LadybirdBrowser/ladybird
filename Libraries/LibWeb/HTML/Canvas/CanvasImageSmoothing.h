/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/ImageData.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasimagesmoothing
class CanvasImageSmoothing : virtual public AbstractCanvasRenderingContext2DBase {
public:
    bool image_smoothing_enabled() const;
    void set_image_smoothing_enabled(bool);
    Bindings::ImageSmoothingQuality image_smoothing_quality() const;
    void set_image_smoothing_quality(Bindings::ImageSmoothingQuality);

protected:
    CanvasImageSmoothing() = default;
};

}
