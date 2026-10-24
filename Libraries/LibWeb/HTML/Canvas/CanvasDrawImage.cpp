/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/Layout/ImageProvider.h>

namespace Web::HTML {

static bool image_provider_is_usable_for_canvas(Layout::ImageProvider const& image_provider)
{
    if (!image_provider.is_image_available())
        return false;

    auto intrinsic_width = image_provider.intrinsic_width();
    auto intrinsic_height = image_provider.intrinsic_height();
    return intrinsic_width.has_value() && intrinsic_height.has_value()
        && *intrinsic_width > 0 && *intrinsic_height > 0;
}

// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
WebIDL::ExceptionOr<CanvasImageSourceUsability> check_usability_of_image(CanvasImageSource const& image)
{
    // 1. Switch on image:
    auto usability = TRY(image.visit(
        // HTMLOrSVGImageElement
        [](GC::Ref<HTMLImageElement> image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's current request's state is broken, then throw an "InvalidStateError" DOMException.
            if (image_element->current_request().state() == HTML::ImageRequest::State::Broken)
                return WebIDL::InvalidStateError::create("Image element state is broken"_utf16);

            // If image is not fully decodable, then return bad.
            // If image has a natural width or natural height (or both) equal to zero, then return bad.
            // NB: An image whose current request is not completely available is not fully decodable, even while the
            //     request still holds image data.
            if (image_element->current_request().state() != HTML::ImageRequest::State::CompletelyAvailable || !image_provider_is_usable_for_canvas(*image_element))
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },
        // FIXME: Don't duplicate this for HTMLImageElement and SVGImageElement.
        [](GC::Ref<SVG::SVGImageElement> image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            // If image has a natural width or natural height (or both) equal to zero, then return bad.
            if (!image_provider_is_usable_for_canvas(*image_element))
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },

        [](GC::Ref<HTML::HTMLVideoElement> video_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's readyState attribute is either HAVE_NOTHING or HAVE_METADATA, then return bad.
            if (video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveNothing || video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveMetadata) {
                return { CanvasImageSourceUsability::Bad };
            }
            return Optional<CanvasImageSourceUsability> {};
        },

        // OffscreenCanvas
        [](GC::Ref<OffscreenCanvas> offscreen_canvas) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (offscreen_canvas->width() == 0 || offscreen_canvas->height() == 0)
                return WebIDL::InvalidStateError::create("OffscreenCanvas width or height is zero"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        },
        // HTMLCanvasElement
        [](GC::Ref<HTMLCanvasElement> canvas_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (canvas_element->width() == 0 || canvas_element->height() == 0)
                return WebIDL::InvalidStateError::create("Canvas width or height is zero"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        },

        // ImageBitmap
        // FIXME: VideoFrame
        [](GC::Ref<ImageBitmap> image_bitmap) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            if (image_bitmap->is_detached())
                return WebIDL::InvalidStateError::create("Image bitmap is detached"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        }));
    if (usability.has_value())
        return usability.release_value();

    // 2. Return good.
    return { CanvasImageSourceUsability::Good };
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
bool image_is_not_origin_clean(CanvasImageSource const& image)
{
    // An object image is not origin-clean if, switching on image's type:
    return image.visit(
        // HTMLOrSVGImageElement
        [](GC::Ref<HTMLImageElement> image) {
            // image's current request's image data is CORS-cross-origin.
            // NB: WebGL asks before it knows that the image has image data.
            auto image_data = image->current_request().image_data();
            return image_data && image_data->is_cors_cross_origin();
        },
        [](GC::Ref<SVG::SVGImageElement> image) {
            // image's current request's image data is CORS-cross-origin.
            auto image_data = image->decoded_image_data();
            return image_data && image_data->is_cors_cross_origin();
        },
        // HTMLVideoElement
        [](GC::Ref<HTMLVideoElement> video) {
            // image's media data is CORS-cross-origin.
            return video->media_data_is_cors_cross_origin();
        },
        // HTMLCanvasElement
        // OffscreenCanvas
        [](OneOf<GC::Ref<HTMLCanvasElement>, GC::Ref<OffscreenCanvas>> auto const& canvas) {
            // image's bitmap's origin-clean flag is false.
            return !canvas->is_origin_clean();
        },
        // ImageBitmap
        [](GC::Ref<ImageBitmap> image_bitmap) {
            // image's bitmap's origin-clean flag is false.
            return !image_bitmap->is_origin_clean();
        });
}

}
