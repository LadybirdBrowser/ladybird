/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::WebGL {

// FIXME: This object should inherit from Bindings::PlatformObject and implement the WebGLRenderingContextBase IDL interface.
//        We should make WebGL code generator to produce implementation for this interface.
class WebGLRenderingContextBase {
public:
    virtual GC::Cell const* gc_cell() const = 0;
    virtual void visit_edges(JS::Cell::Visitor&) = 0;
    virtual OpenGLContext& context() = 0;
};

}
