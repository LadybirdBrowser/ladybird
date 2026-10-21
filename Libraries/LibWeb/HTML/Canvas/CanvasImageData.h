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

    virtual WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(int width, int height, Optional<ImageData::Settings> const& settings = {}) const = 0;
    virtual WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(ImageData const&) const = 0;
    virtual WebIDL::ExceptionOr<GC::Ptr<ImageData>> get_image_data(int x, int y, int width, int height, Optional<ImageData::Settings> const& settings = {}) = 0;
    virtual WebIDL::ExceptionOr<void> put_image_data(ImageData&, int x, int y) = 0;
    virtual WebIDL::ExceptionOr<void> put_image_data(ImageData&, int x, int y, int dirty_x, int dirty_y, int dirty_width, int dirty_height) = 0;

protected:
    CanvasImageData() = default;
};

}
