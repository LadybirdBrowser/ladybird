/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

// https://html.spec.whatwg.org/multipage/canvas.html#canvasimagesource
// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using CanvasImageSource = Variant<GC::Root<Web::HTML::HTMLImageElement>, GC::Root<Web::SVG::SVGImageElement>, GC::Root<Web::HTML::HTMLCanvasElement>, GC::Root<Web::HTML::ImageBitmap>, GC::Root<Web::HTML::OffscreenCanvas>, GC::Root<Web::HTML::HTMLVideoElement>>;

enum class CanvasImageSourceUsability {
    Bad,
    Good,
};

#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/SVG/SVGImageElement.h>
