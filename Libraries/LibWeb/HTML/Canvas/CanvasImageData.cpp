/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Painter.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/Canvas/CanvasImageData.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context2d-putimagedata-common
WebIDL::ExceptionOr<void> CanvasImageData::put_pixels_from_an_image_data_onto_a_bitmap(ImageData& image_data, Gfx::Painter& painter, float dx, float dy, float dirty_x, float dirty_y, float dirty_width, float dirty_height)
{
    // 1. Let buffer be imageData's data attribute value's [[ViewedArrayBuffer]] internal slot.
    auto* buffer = image_data.data()->viewed_array_buffer();

    // 2. If IsDetachedBuffer(buffer) is true, then throw an "InvalidStateError" DOMException
    if (buffer->is_detached())
        return WebIDL::InvalidStateError::create(image_data.realm(), "ImageData's underlying buffer is detached"_utf16);

    // 3. If dirtyWidth is negative, then let dirtyX be dirtyX+dirtyWidth, and let dirtyWidth be equal to the
    //    absolute magnitude of dirtyWidth.
    if (dirty_width < 0) {
        dirty_x += dirty_width;
        dirty_width = abs(dirty_width);
    }
    // If dirtyHeight is negative, then let dirtyY be dirtyY+dirtyHeight, and let dirtyHeight be equal to the absolute
    // magnitude of dirtyHeight.
    if (dirty_height < 0) {
        dirty_y += dirty_height;
        dirty_height = abs(dirty_height);
    }

    // 4. If dirtyX is negative, then let dirtyWidth be dirtyWidth+dirtyX, and let dirtyX be 0.
    if (dirty_x < 0) {
        dirty_width += dirty_x;
        dirty_x = 0;
    }

    // If dirtyY is negative, then let dirtyHeight be dirtyHeight+dirtyY, and let dirtyY be 0.
    if (dirty_y < 0) {
        dirty_height += dirty_y;
        dirty_y = 0;
    }

    // 5. If dirtyX+dirtyWidth is greater than the width attribute of the imageData argument, then let dirtyWidth be
    //    the value of that width attribute, minus the value of dirtyX.
    if (dirty_x + dirty_width > image_data.width()) {
        dirty_width = image_data.width() - dirty_x;
    }
    // If dirtyY+dirtyHeight is greater than the height attribute of the imageData argument, then let dirtyHeight be
    // the value of that height attribute, minus the value of dirtyY.
    if (dirty_y + dirty_height > image_data.height()) {
        dirty_height = image_data.height() - dirty_y;
    }

    // 6. If, after those changes, either dirtyWidth or dirtyHeight are negative or zero, then return without affecting
    //    any bitmaps.
    if (dirty_width <= 0 || dirty_height <= 0)
        return {};

    // 7. For all integer values of x and y where dirtyX ≤ x < dirtyX+dirtyWidth and dirtyY ≤ y < dirtyY+dirtyHeight,
    //    set the pixel with coordinate (dx+x, dy+y) in bitmap to the color of the pixel at coordinate (x, y) in the
    //    imageData data structure's bitmap, converted from imageData's colorSpace to the color space of bitmap using
    //    'relative-colorimetric' rendering intent.
    auto dst_rect = Gfx::FloatRect { dx + dirty_x, dy + dirty_y, dirty_width, dirty_height };
    painter.save();
    painter.set_transform({});
    painter.draw_bitmap(
        dst_rect,
        Gfx::ImmutableBitmap::create(image_data.bitmap(), Gfx::AlphaType::Unpremultiplied),
        Gfx::IntRect { dirty_x, dirty_y, dirty_width, dirty_height },
        Gfx::ScalingMode::NearestNeighbor,
        {},
        1.0f,
        Gfx::CompositingAndBlendingOperator::SourceOver);
    painter.restore();

    did_draw(dst_rect);

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> CanvasImageData::create_image_data(int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    int abs_width = abs(width);
    int abs_height = abs(height);

    // 2. Let newImageData be a new ImageData object.
    // 3. Initialize newImageData given the absolute magnitude of sw, the absolute magnitude of sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // 4. Initialize the image data of newImageData to transparent black.
    // ... this is handled by ImageData::create()

    // 5. Return newImageData.
    return image_data;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata-imagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> CanvasImageData::create_image_data(ImageData const& image_data) const
{
    // 1. Let newImageData be a new ImageData object.
    // 2. Initialize newImageData given the value of imageData's width attribute, the value of imageData's height attribute, and defaultColorSpace set to the value of imageData's colorSpace attribute.
    // FIXME: Set defaultColorSpace to the value of image_data's colorSpace attribute
    // 3. Initialize the image data of newImageData to transparent black.
    // NOTE: No-op, already done during creation.
    // 4. Return newImageData.
    return TRY(ImageData::create(realm(), image_data.width(), image_data.height()));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getimagedata
WebIDL::ExceptionOr<GC::Ptr<ImageData>> CanvasImageData::get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings) const
{
    // 1. If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    // 2. If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!origin_clean())
        return WebIDL::SecurityError::create(realm(), "CanvasRenderingContext2D is not origin-clean"_utf16);

    // ImageData initialization requires positive width and height
    // https://html.spec.whatwg.org/multipage/canvas.html#initialize-an-imagedata-object
    int abs_width = abs(width);
    int abs_height = abs(height);

    // 3. Let imageData be a new ImageData object.
    // 4. Initialize imageData given sw, sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // NOTE: We don't attempt to create the underlying bitmap here; if it doesn't exist, it's like copying only transparent black pixels (which is a no-op).
    if (!surface())
        return image_data;
    auto const snapshot = Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface());

    // 5. Let the source rectangle be the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::Rect { x, y, abs_width, abs_height };

    // NOTE: The spec doesn't seem to define this behavior, but MDN does and the WPT tests
    // assume it works this way.
    // https://developer.mozilla.org/en-US/docs/Web/API/CanvasRenderingContext2D/getImageData#sw
    if (width < 0 || height < 0) {
        source_rect = source_rect.translated(min(width, 0), min(height, 0));
    }
    auto source_rect_intersected = source_rect.intersected(snapshot->rect());

    // 6. Set the pixel values of imageData to be the pixels of this's output bitmap in the area specified by the source rectangle in the bitmap's coordinate space units, converted from this's color space to imageData's colorSpace using 'relative-colorimetric' rendering intent.
    // NOTE: Internally we must use premultiplied alpha, but ImageData should hold unpremultiplied alpha. This conversion
    //       might result in a loss of precision, but is according to spec.
    //       See: https://html.spec.whatwg.org/multipage/canvas.html#premultiplied-alpha-and-the-2d-rendering-context
    VERIFY(snapshot->alpha_type() == Gfx::AlphaType::Premultiplied);
    VERIFY(image_data->bitmap().alpha_type() == Gfx::AlphaType::Unpremultiplied);

    auto painter = Gfx::Painter::create(image_data->bitmap());
    painter->draw_bitmap(image_data->bitmap().rect().template to_type<float>(), *snapshot, source_rect_intersected, Gfx::ScalingMode::NearestNeighbor, {}, 1, Gfx::CompositingAndBlendingOperator::SourceOver);

    // 7. Set the pixels values of imageData for areas of the source rectangle that are outside of the output bitmap to transparent black.
    // NOTE: No-op, already done during creation.

    // 8. Return imageData.
    return image_data;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata-short
WebIDL::ExceptionOr<void> CanvasImageData::put_image_data(ImageData& image_data, float dx, float dy)
{
    // The putImageData(imageData, dx, dy) method steps are to put pixels from an ImageData onto a bitmap,
    // given imageData, this's output bitmap, dx, dy, 0, 0, imageData's width, and imageData's height.
    if (auto* painter = this->painter())
        TRY(put_pixels_from_an_image_data_onto_a_bitmap(image_data, *painter, dx, dy, 0, 0, image_data.width(), image_data.height()));

    return {};
}
WebIDL::ExceptionOr<void> CanvasImageData::put_image_data(ImageData& image_data, float dx, float dy, float dirty_x, float dirty_y, float dirty_width, float dirty_height)
{
    // The putImageData(imageData, dx, dy) method steps are to put pixels from an ImageData onto a bitmap,
    // given imageData, this's output bitmap, dx, dy, 0, 0, imageData's width, and imageData's height.
    if (auto* painter = this->painter())
        TRY(put_pixels_from_an_image_data_onto_a_bitmap(image_data, *painter, dx, dy, dirty_x, dirty_y, dirty_width, dirty_height));

    return {};
}

}
