/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGImageElement.h>

namespace Web::HTML {

static void default_source_size(CanvasImageSource const& image, float& source_width, float& source_height)
{
    image.visit(
        [&source_width, &source_height](GC::Root<HTMLImageElement> const& source) {
            if (source->immutable_bitmap()) {
                source_width = source->immutable_bitmap()->width(Gfx::ImageOrientation::FromDecoded);
                source_height = source->immutable_bitmap()->height(Gfx::ImageOrientation::FromDecoded);
            } else {
                // FIXME: This is very janky and not correct.
                source_width = source->width();
                source_height = source->height();
            }
        },
        [&source_width, &source_height](GC::Root<SVG::SVGImageElement> const& source) {
            if (source->current_image_bitmap()) {
                source_width = source->current_image_bitmap()->width(Gfx::ImageOrientation::FromDecoded);
                source_height = source->current_image_bitmap()->height(Gfx::ImageOrientation::FromDecoded);
            } else {
                // FIXME: This is very janky and not correct.
                source_width = source->width()->anim_val()->value();
                source_height = source->height()->anim_val()->value();
            }
        },
        [&source_width, &source_height](GC::Root<HTML::HTMLVideoElement> const& source) {
            if (auto const bitmap = source->bitmap(); bitmap) {
                source_width = bitmap->width(Gfx::ImageOrientation::FromDecoded);
                source_height = bitmap->height(Gfx::ImageOrientation::FromDecoded);
            } else {
                source_width = source->video_width();
                source_height = source->video_height();
            }
        },
        [&source_width, &source_height](GC::Root<OffscreenCanvas> const& source) {
            auto const bitmap = source->bitmap();

            if (!bitmap) {
                source_width = 0;
                source_height = 0;
                return;
            }
            source_width = bitmap->width();
            source_height = bitmap->height();
        },
        [&source_width, &source_height](GC::Root<HTMLCanvasElement> const& source) {
            if (source->surface()) {
                source_width = source->surface()->size().width();
                source_height = source->surface()->size().height();
            } else {
                source_width = source->width();
                source_height = source->height();
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

Gfx::ImageOrientation get_image_orientation_from_canvas_source(CanvasImageSource const& source)
{
    return source.visit(
        [](auto const& src) -> Gfx::ImageOrientation {
            auto const& computed_properties = src->computed_properties();
            return computed_properties
                ? computed_properties->image_orientation()
                : Gfx::ImageOrientation::FromDecoded;
        },
        [](GC::Root<OffscreenCanvas> const &) -> Gfx::ImageOrientation { return Gfx::ImageOrientation::FromDecoded; },
        [](GC::Root<ImageBitmap> const&) -> Gfx::ImageOrientation { return Gfx::ImageOrientation::FromDecoded; });
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
