/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/Screenshot.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-draw-a-bounding-box-from-the-framebuffer
ErrorOr<GC::Ref<HTML::HTMLCanvasElement>, WebDriver::Error> draw_bounding_box_from_the_framebuffer(HTML::BrowsingContext& browsing_context, DOM::Element& element, Gfx::IntRect rect)
{
    // 1. If either the initial viewport's width or height is 0 CSS pixels, return error with error code unable to capture screen.
    auto viewport_rect = browsing_context.top_level_traversable()->viewport_rect();
    if (viewport_rect.is_empty())
        return Error::from_code(ErrorCode::UnableToCaptureScreen, "Viewport is empty"sv);

    auto viewport_device_rect = browsing_context.page().enclosing_device_rect(viewport_rect).to_type<int>();

    // 2. Let paint width be the initial viewport's width – min(rectangle x coordinate, rectangle x coordinate + rectangle width dimension).
    auto paint_width = viewport_device_rect.width() - min(rect.x(), rect.x() + rect.width());

    // 3. Let paint height be the initial viewport's height – min(rectangle y coordinate, rectangle y coordinate + rectangle height dimension).
    auto paint_height = viewport_device_rect.height() - min(rect.y(), rect.y() + rect.height());

    // 4. Let canvas be a new canvas element, and set its width and height to paint width and paint height, respectively.
    auto canvas_element = DOM::create_element(element.document(), HTML::TagNames::canvas, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    auto& canvas = as<HTML::HTMLCanvasElement>(*canvas_element);

    // FIXME: Handle DevicePixelRatio in HiDPI mode.
    MUST(canvas.set_width(paint_width));
    MUST(canvas.set_height(paint_height));

    // FIXME: 5. Let context, a canvas context mode, be the result of invoking the 2D context creation algorithm given canvas as the target.
    canvas.create_2d_context();
    canvas.allocate_painting_surface_if_needed();
    if (!canvas.surface())
        return Error::from_code(ErrorCode::UnableToCaptureScreen, "Failed to allocate painting surface"sv);

    // 6. Complete implementation specific steps equivalent to drawing the region of the framebuffer specified by the following coordinates onto context:
    //    - X coordinate: rectangle x coordinate
    //    - Y coordinate: rectangle y coordinate
    //    - Width: paint width
    //    - Height: paint height
    Gfx::IntRect paint_rect { rect.x(), rect.y(), paint_width, paint_height };

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, canvas.surface()->size()));
    auto backing_store = Web::Painting::BitmapBackingStore(bitmap);
    browsing_context.page().client().paint(paint_rect.to_type<Web::DevicePixels>(), backing_store);
    canvas.surface()->write_from_bitmap(*bitmap);

    // 7. Return success with canvas.
    return canvas;
}

// https://w3c.github.io/webdriver/#dfn-encoding-a-canvas-as-base64
Response encode_canvas_element(HTML::HTMLCanvasElement& canvas)
{
    // FIXME: 1. If the canvas element’s bitmap’s origin-clean flag is set to false, return error with error code unable to capture screen.

    // 2. If the canvas element’s bitmap has no pixels (i.e. either its horizontal dimension or vertical dimension is zero) then return error with error code unable to capture screen.
    if (canvas.surface()->size().is_empty())
        return Error::from_code(ErrorCode::UnableToCaptureScreen, "Captured screenshot is empty"sv);

    // 3. Let file be a serialization of the canvas element’s bitmap as a file, using "image/png" as an argument.
    // 4. Let data url be a data: URL representing file. [RFC2397]
    auto data_url = canvas.to_data_url("image/png"sv, JS::js_undefined());

    // 5. Let index be the index of "," in data url.
    auto index = data_url.find_byte_offset(',');
    VERIFY(index.has_value());

    // 6. Let encoded string be a substring of data url using (index + 1) as the start argument.
    auto encoded_string = MUST(data_url.substring_from_byte_offset(*index + 1));

    // 7. Return success with data encoded string.
    return JsonValue { encoded_string.to_byte_string() };
}

}
