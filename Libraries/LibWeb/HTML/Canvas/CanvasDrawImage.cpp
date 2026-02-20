/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Painter.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>
#include <LibWeb/HTML/CheckUsabilityOfImage.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageIsNotOriginClean.h>

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

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image_internal(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(source_x) || !isfinite(source_y) || !isfinite(source_width) || !isfinite(source_height) || !isfinite(destination_x) || !isfinite(destination_y) || !isfinite(destination_width) || !isfinite(destination_height))
        return {};

    // 2. Let usability be the result of checking the usability of image.
    auto usability = TRY(check_usability_of_image(image));

    // 3. If usability is bad, then return (without drawing anything).
    if (usability == CanvasImageSourceUsability::Bad)
        return {};

    auto bitmap = canvas_image_source_bitmap(image);
    if (!bitmap)
        return {};

    // 4. Establish the source and destination rectangles as follows:
    //    If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
    //    If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    //    If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    //    neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy), (dx+dw, dy+dh), (dx, dy+dh).
    // NOTE: Implemented in drawImage() overloads
    if (source_width < 0) {
        source_x += source_width;
        source_width = abs(source_width);
    }
    if (source_height < 0) {
        source_y += source_height;
        source_height = abs(source_height);
    }
    if (destination_width < 0) {
        destination_x += destination_width;
        destination_width = abs(destination_width);
    }
    if (destination_height < 0) {
        destination_y += destination_height;
        destination_height = abs(destination_height);
    }

    //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::FloatRect { source_x, source_y, source_width, source_height };
    //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy), (dx+dw, dy+dh), (dx, dy+dh).
    auto destination_rect = Gfx::FloatRect { destination_x, destination_y, destination_width, destination_height };
    //    When the source rectangle is outside the source image, the source rectangle must be clipped
    //    to the source image and the destination rectangle must be clipped in the same proportion.
    auto clipped_source = source_rect.intersected(bitmap->rect().to_type<float>());
    auto clipped_destination = destination_rect;
    if (clipped_source != source_rect) {
        clipped_destination.set_width(clipped_destination.width() * (clipped_source.width() / source_rect.width()));
        clipped_destination.set_height(clipped_destination.height() * (clipped_source.height() / source_rect.height()));
    }

    // 5. If one of the sw or sh arguments is zero, then return. Nothing is painted.
    if (source_width == 0 || source_height == 0)
        return {};

    // 6. Paint the region of the image argument specified by the source rectangle on the region of the rendering context's output bitmap specified by the destination rectangle, after applying the current transformation matrix to the destination rectangle.
    auto scaling_mode = Gfx::ScalingMode::NearestNeighbor;
    if (drawing_state().image_smoothing_enabled) {
        // FIXME: Honor drawing_state().image_smoothing_quality
        scaling_mode = Gfx::ScalingMode::BilinearMipmap;
    }

    if (auto* painter = this->painter()) {
        painter->draw_bitmap(destination_rect, *bitmap, source_rect.to_rounded<int>(), scaling_mode, drawing_state().filter, drawing_state().global_alpha, drawing_state().current_compositing_and_blending_operator);
        did_draw(destination_rect);
    }

    // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
    if (image_is_not_origin_clean(image))
        m_origin_clean = false;

    return {};
}

}
