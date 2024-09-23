/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Rect.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/AnimationFrameCallbackDriver.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Screenshot.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-encoding-a-canvas-as-base64
static Response encode_canvas_element(HTML::HTMLCanvasElement& canvas)
{
    // FIXME: 1. If the canvas element’s bitmap’s origin-clean flag is set to false, return error with error code unable to capture screen.

    // 2. If the canvas element’s bitmap has no pixels (i.e. either its horizontal dimension or vertical dimension is zero) then return error with error code unable to capture screen.
    if (canvas.bitmap()->width() == 0 || canvas.bitmap()->height() == 0)
        return Error::from_code(ErrorCode::UnableToCaptureScreen, "Captured screenshot is empty"sv);

    // 3. Let file be a serialization of the canvas element’s bitmap as a file, using "image/png" as an argument.
    // 4. Let data url be a data: URL representing file. [RFC2397]
    auto data_url = canvas.to_data_url("image/png"sv, {});

    // 5. Let index be the index of "," in data url.
    auto index = data_url.find_byte_offset(',');
    VERIFY(index.has_value());

    // 6. Let encoded string be a substring of data url using (index + 1) as the start argument.
    auto encoded_string = MUST(data_url.substring_from_byte_offset(*index + 1));

    // 7. Return success with data encoded string.
    return JsonValue { move(encoded_string) };
}

// Common animation callback steps between:
// https://w3c.github.io/webdriver/#take-screenshot
// https://w3c.github.io/webdriver/#take-element-screenshot
Response capture_element_screenshot(Painter const& painter, Page& page, DOM::Element& element, Gfx::IntRect& rect)
{
    Optional<Response> encoded_string_or_error;

    // https://w3c.github.io/webdriver/#dfn-draw-a-bounding-box-from-the-framebuffer
    auto draw_bounding_box_from_the_framebuffer = [&]() -> ErrorOr<JS::NonnullGCPtr<HTML::HTMLCanvasElement>, WebDriver::Error> {
        // 1. If either the initial viewport's width or height is 0 CSS pixels, return error with error code unable to capture screen.
        auto viewport_rect = page.top_level_traversable()->viewport_rect();
        if (viewport_rect.is_empty())
            return Error::from_code(ErrorCode::UnableToCaptureScreen, "Viewport is empty"sv);

        auto viewport_device_rect = page.enclosing_device_rect(viewport_rect).to_type<int>();

        // 2. Let paint width be the initial viewport's width – min(rectangle x coordinate, rectangle x coordinate + rectangle width dimension).
        auto paint_width = viewport_device_rect.width() - min(rect.x(), rect.x() + rect.width());

        // 3. Let paint height be the initial viewport's height – min(rectangle y coordinate, rectangle y coordinate + rectangle height dimension).
        auto paint_height = viewport_device_rect.height() - min(rect.y(), rect.y() + rect.height());

        // 4. Let canvas be a new canvas element, and set its width and height to paint width and paint height, respectively.
        auto canvas_element = DOM::create_element(element.document(), HTML::TagNames::canvas, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
        auto& canvas = verify_cast<HTML::HTMLCanvasElement>(*canvas_element);

        // FIXME: Handle DevicePixelRatio in HiDPI mode.
        MUST(canvas.set_width(paint_width));
        MUST(canvas.set_height(paint_height));

        // FIXME: 5. Let context, a canvas context mode, be the result of invoking the 2D context creation algorithm given canvas as the target.
        if (!canvas.create_bitmap(paint_width, paint_height))
            return Error::from_code(ErrorCode::UnableToCaptureScreen, "Unable to create a screenshot bitmap"sv);

        // 6. Complete implementation specific steps equivalent to drawing the region of the framebuffer specified by the following coordinates onto context:
        //    - X coordinate: rectangle x coordinate
        //    - Y coordinate: rectangle y coordinate
        //    - Width: paint width
        //    - Height: paint height
        Gfx::IntRect paint_rect { rect.x(), rect.y(), paint_width, paint_height };
        painter(paint_rect, *canvas.bitmap());

        // 7. Return success with canvas.
        return canvas;
    };

    (void)element.document().window()->animation_frame_callback_driver().add([&](auto) {
        auto canvas_or_error = draw_bounding_box_from_the_framebuffer();
        if (canvas_or_error.is_error()) {
            encoded_string_or_error = canvas_or_error.release_error();
            return;
        }
        encoded_string_or_error = encode_canvas_element(canvas_or_error.release_value());
    });

    Platform::EventLoopPlugin::the().spin_until([&]() { return encoded_string_or_error.has_value(); });
    return encoded_string_or_error.release_value();
}

}
