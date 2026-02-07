/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Painter.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/ImageData.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasimagedata
class CanvasImageData : virtual public AbstractCanvasRenderingContext2DBase {
public:
    WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(int width, int height, Optional<ImageDataSettings> const& settings = {}) const;
    WebIDL::ExceptionOr<GC::Ref<ImageData>> create_image_data(ImageData const&) const;
    WebIDL::ExceptionOr<GC::Ptr<ImageData>> get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings = {}) const;
    WebIDL::ExceptionOr<void> put_image_data(ImageData&, float x, float y);
    WebIDL::ExceptionOr<void> put_image_data(ImageData&, float x, float y, float dirty_x, float dirty_y, float dirty_width, float dirty_height);

protected:
    CanvasImageData() = default;

private:
    WebIDL::ExceptionOr<void> put_pixels_from_an_image_data_onto_a_bitmap(ImageData& image_data, Gfx::Painter& painter, float dx, float dy, float dirty_x, float dirty_y, float dirty_width, float dirty_height);
};

}
