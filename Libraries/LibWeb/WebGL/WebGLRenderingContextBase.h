/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebGL {

// NOTE: This is the Variant created by the IDL wrapper generator, and needs to be updated accordingly.
using TexImageSource = Variant<GC::Root<HTML::ImageBitmap>, GC::Root<HTML::ImageData>, GC::Root<HTML::HTMLImageElement>, GC::Root<HTML::HTMLCanvasElement>, GC::Root<HTML::OffscreenCanvas>, GC::Root<HTML::HTMLVideoElement>>;

// FIXME: This object should inherit from Bindings::PlatformObject and implement the WebGLRenderingContextBase IDL interface.
//        We should make WebGL code generator to produce implementation for this interface.
class WebGLRenderingContextBase {
public:
    using Float32List = Variant<GC::Root<JS::Float32Array>, Vector<float>>;

    virtual GC::Cell const* gc_cell() const = 0;
    virtual void visit_edges(JS::Cell::Visitor&) = 0;
    virtual OpenGLContext& context() = 0;
};

}
