/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

// https://registry.khronos.org/webgl/specs/latest/1.0/#5.14
class WEB_API WebGLRenderingContext final : public WebGLRenderingContextBase {
    WEB_RUST_INTERFACE(WebGLRenderingContext, web_gl_rendering_context)

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> create(JS::Realm&, CanvasOwner, JS::Value options);
};

}
