/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

// https://registry.khronos.org/webgl/specs/latest/2.0/#3.7
class WEB_API WebGL2RenderingContext final : public WebGLRenderingContextBase {
    WEB_RUST_INTERFACE(WebGL2RenderingContext, web_gl2_rendering_context)

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGL2RenderingContext>> create(JS::Realm&, CanvasOwner, JS::Value options);
};

}
