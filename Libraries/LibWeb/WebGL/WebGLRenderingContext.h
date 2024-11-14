/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

class WebGLRenderingContext final : public WebGLRenderingContextBase {
    WEB_PLATFORM_OBJECT(WebGLRenderingContext, WebGLRenderingContextBase);
    GC_DECLARE_ALLOCATOR(WebGLRenderingContext);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> create(JS::Realm&, HTML::HTMLCanvasElement& canvas_element, JS::Value options);

    virtual ~WebGLRenderingContext() override;

private:
    virtual void initialize(JS::Realm&) override;

    WebGLRenderingContext(JS::Realm&, HTML::HTMLCanvasElement&, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters);
};

}
