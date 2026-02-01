/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
#include <LibWeb/HTML/Canvas/CanvasImageSource.h>
#include <LibWeb/HTML/ImageRequest.h>

#pragma once

namespace Web::HTML {

[[maybe_unused]] static WebIDL::ExceptionOr<CanvasImageSourceUsability> check_usability_of_image(CanvasImageSource const& image)
{
    // 1. Switch on image:
    auto usability = TRY(image.visit(
        // HTMLOrSVGImageElement
        [](GC::Root<HTMLImageElement> const& image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's current request's state is broken, then throw an "InvalidStateError" DOMException.
            if (image_element->current_request().state() == HTML::ImageRequest::State::Broken)
                return WebIDL::InvalidStateError::create(image_element->realm(), "Image element state is broken"_utf16);

            // If image is not fully decodable, then return bad.
            if (!image_element->immutable_bitmap())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (image_element->immutable_bitmap()->width() == 0 || image_element->immutable_bitmap()->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },
        // FIXME: Don't duplicate this for HTMLImageElement and SVGImageElement.
        [](GC::Root<SVG::SVGImageElement> const& image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            if (!image_element->current_image_bitmap())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (image_element->current_image_bitmap()->width() == 0 || image_element->current_image_bitmap()->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },

        [](GC::Root<HTML::HTMLVideoElement> const& video_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's readyState attribute is either HAVE_NOTHING or HAVE_METADATA, then return bad.
            if (video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveNothing || video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveMetadata) {
                return { CanvasImageSourceUsability::Bad };
            }
            return Optional<CanvasImageSourceUsability> {};
        },

        // OffscreenCanvas
        [](GC::Root<OffscreenCanvas> const& offscreen_canvas) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (offscreen_canvas->width() == 0 || offscreen_canvas->height() == 0)
                return WebIDL::InvalidStateError::create(offscreen_canvas->realm(), "OffscreenCanvas width or height is zero"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        },
        // HTMLCanvasElement
        [](GC::Root<HTMLCanvasElement> const& canvas_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (canvas_element->width() == 0 || canvas_element->height() == 0)
                return WebIDL::InvalidStateError::create(canvas_element->realm(), "Canvas width or height is zero"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        },

        // ImageBitmap
        // FIXME: VideoFrame
        [](GC::Root<ImageBitmap> const& image_bitmap) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            if (image_bitmap->is_detached())
                return WebIDL::InvalidStateError::create(image_bitmap->realm(), "Image bitmap is detached"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        }));
    if (usability.has_value())
        return usability.release_value();

    // 2. Return good.
    return { CanvasImageSourceUsability::Good };
}

}
