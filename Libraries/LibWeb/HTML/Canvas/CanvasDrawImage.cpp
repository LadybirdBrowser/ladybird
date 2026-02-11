/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/ImageBitmap.h>

namespace Web::HTML {

Gfx::IntSize canvas_image_source_dimensions(CanvasImageSource const& image)
{
    return image.visit(
        [](GC::Root<HTMLImageElement> const& source) -> Gfx::IntSize {
            if (auto immutable_bitmap = source->immutable_bitmap())
                return immutable_bitmap->size();

            // FIXME: This is very janky and not correct.
            return { source->width(), source->height() };
        },
        [](GC::Root<SVG::SVGImageElement> const& source) -> Gfx::IntSize {
            if (auto immutable_bitmap = source->current_image_bitmap())
                return immutable_bitmap->size();

            // FIXME: This is very janky and not correct.
            return { source->width()->anim_val()->value(), source->height()->anim_val()->value() };
        },
        [](GC::Root<HTMLCanvasElement> const& source) -> Gfx::IntSize {
            if (auto painting_surface = source->surface())
                return painting_surface->size();
            return { source->width(), source->height() };
        },
        [](GC::Root<ImageBitmap> const& source) -> Gfx::IntSize {
            if (auto* bitmap = source->bitmap())
                return bitmap->size();
            return { source->width(), source->height() };
        },
        [](GC::Root<OffscreenCanvas> const& source) -> Gfx::IntSize {
            if (auto bitmap = source->bitmap())
                return bitmap->size();
            return {};
        },
        [](GC::Root<HTMLVideoElement> const& source) -> Gfx::IntSize {
            if (auto bitmap = source->bitmap())
                return bitmap->size();
            return { source->video_width(), source->video_height() };
        });
}

RefPtr<Gfx::ImmutableBitmap> canvas_image_source_bitmap(CanvasImageSource const& image)
{
    return image.visit(
        [](OneOf<GC::Root<HTMLImageElement>, GC::Root<SVG::SVGImageElement>> auto const& element) {
            return element->default_image_bitmap();
        },
        [](GC::Root<HTMLCanvasElement> const& canvas) -> RefPtr<Gfx::ImmutableBitmap> {
            canvas->present();
            auto surface = canvas->surface();
            if (!surface)
                return Gfx::ImmutableBitmap::create(*canvas->get_bitmap_from_surface());
            return Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface);
        },
        [](OneOf<GC::Root<ImageBitmap>, GC::Root<OffscreenCanvas>> auto const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            auto bitmap = source->bitmap();
            if (!bitmap)
                return {};
            return Gfx::ImmutableBitmap::create(*bitmap);
        },
        [](GC::Root<HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->bitmap();
        });
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(CanvasImageSource const& image, float destination_x, float destination_y)
{
    // If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
    // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    // If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    // neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    auto size = canvas_image_source_dimensions(image);
    return draw_image_internal(image, 0, 0, size.width(), size.height(), destination_x, destination_y, size.width(), size.height());
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(CanvasImageSource const& image, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    // If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    // neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    auto size = canvas_image_source_dimensions(image);
    return draw_image_internal(image, 0, 0, size.width(), size.height(), destination_x, destination_y, destination_width, destination_height);
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    return draw_image_internal(image, source_x, source_y, source_width, source_height, destination_x, destination_y, destination_width, destination_height);
}

}
