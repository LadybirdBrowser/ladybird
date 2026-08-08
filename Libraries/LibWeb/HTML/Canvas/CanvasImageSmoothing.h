/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ImageData.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasimagesmoothing
class CanvasImageSmoothing {
public:
    virtual ~CanvasImageSmoothing() = default;

    virtual bool image_smoothing_enabled() const = 0;
    virtual void set_image_smoothing_enabled(bool) = 0;
    virtual ImageSmoothingQuality image_smoothing_quality() const = 0;
    virtual void set_image_smoothing_quality(ImageSmoothingQuality) = 0;

protected:
    CanvasImageSmoothing() = default;
};

}
