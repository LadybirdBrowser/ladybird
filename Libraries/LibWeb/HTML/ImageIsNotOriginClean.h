/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasImageSource.h>

#pragma once

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
[[maybe_unused]] static bool image_is_not_origin_clean(CanvasImageSource const& image)
{
    // An object image is not origin-clean if, switching on image's type:
    return image.visit(
        // HTMLOrSVGImageElement
        [](GC::Root<HTMLImageElement> const&) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](GC::Root<SVG::SVGImageElement> const&) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](GC::Root<HTML::HTMLVideoElement> const&) {
            // FIXME: image's media data is CORS-cross-origin.
            return false;
        },
        // HTMLCanvasElement, ImageBitmap or OffscreenCanvas
        [](OneOf<GC::Root<HTMLCanvasElement>, GC::Root<ImageBitmap>, GC::Root<OffscreenCanvas>> auto const&) {
            // FIXME: image's bitmap's origin-clean flag is false.
            return false;
        });
}

}
