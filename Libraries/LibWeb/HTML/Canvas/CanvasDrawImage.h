/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasimagesource
// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using CanvasImageSource = Variant<JS::Handle<HTMLImageElement>, JS::Handle<SVG::SVGImageElement>, JS::Handle<HTMLCanvasElement>, JS::Handle<ImageBitmap>, JS::Handle<HTMLVideoElement>>;

// https://html.spec.whatwg.org/multipage/canvas.html#canvasdrawimage
class CanvasDrawImage {
public:
    virtual ~CanvasDrawImage() = default;

    WebIDL::ExceptionOr<void> draw_image(CanvasImageSource const&, float destination_x, float destination_y);
    WebIDL::ExceptionOr<void> draw_image(CanvasImageSource const&, float destination_x, float destination_y, float destination_width, float destination_height);
    WebIDL::ExceptionOr<void> draw_image(CanvasImageSource const&, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height);

    virtual WebIDL::ExceptionOr<void> draw_image_internal(CanvasImageSource const&, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height) = 0;

protected:
    CanvasDrawImage() = default;
};

}
