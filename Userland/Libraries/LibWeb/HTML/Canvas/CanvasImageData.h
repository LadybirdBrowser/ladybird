/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/ImageData.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasimagedata
class CanvasImageData {
public:
    virtual ~CanvasImageData() = default;

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> create_image_data(int width, int height, Optional<ImageDataSettings> const& settings = {}) const = 0;
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> create_image_data(ImageData const&) const = 0;
    virtual WebIDL::ExceptionOr<JS::GCPtr<ImageData>> get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings = {}) const = 0;
    virtual void put_image_data(ImageData const&, float x, float y) = 0;

protected:
    CanvasImageData() = default;
};

}
