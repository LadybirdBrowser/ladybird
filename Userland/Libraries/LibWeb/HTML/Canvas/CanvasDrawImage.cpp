/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/SVG/SVGImageElement.h>

namespace Web::HTML {

static void default_source_size(CanvasImageSource const& image, float& source_width, float& source_height)
{
    image.visit(
        [&source_width, &source_height](JS::Handle<SVG::SVGImageElement> const& source) {
            if (source->bitmap()) {
                source_width = source->bitmap()->width();
                source_height = source->bitmap()->height();
            } else {
                // FIXME: This is very janky and not correct.
                source_width = source->width()->anim_val()->value();
                source_height = source->height()->anim_val()->value();
            }
        },
        [&source_width, &source_height](JS::Handle<HTML::HTMLVideoElement> const& source) {
            if (auto const bitmap = source->bitmap(); bitmap) {
                source_width = bitmap->width();
                source_height = bitmap->height();
            } else {
                source_width = source->video_width();
                source_height = source->video_height();
            }
        },
        [&source_width, &source_height](auto const& source) {
            if (source->bitmap()) {
                source_width = source->bitmap()->width();
                source_height = source->bitmap()->height();
            } else {
                source_width = source->width();
                source_height = source->height();
            }
        });
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(Web::HTML::CanvasImageSource const& image, float destination_x, float destination_y)
{
    // If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
    // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    // If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    // neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    float source_width;
    float source_height;
    default_source_size(image, source_width, source_height);
    return draw_image_internal(image, 0, 0, source_width, source_height, destination_x, destination_y, source_width, source_height);
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(Web::HTML::CanvasImageSource const& image, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    // If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    // neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    float source_width;
    float source_height;
    default_source_size(image, source_width, source_height);
    return draw_image_internal(image, 0, 0, source_width, source_height, destination_x, destination_y, destination_width, destination_height);
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(Web::HTML::CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    return draw_image_internal(image, source_x, source_y, source_width, source_height, destination_x, destination_y, destination_width, destination_height);
}

}
