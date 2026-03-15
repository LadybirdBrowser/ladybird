/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebXR/XRLayer.h>
#include <LibWeb/WebXR/XRSession.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#dictdef-xrwebgllayerinit
struct XRWebGLLayerInit {
    bool antialias { true };
    bool depth { true };
    bool stencil { false };
    bool alpha { true };
    bool ignore_depth_values { false };
    double framebuffer_scale_factor { 1.0 };
};

// https://www.w3.org/TR/webxr/#xrwebgllayer-interface
class XRWebGLLayer : public XRLayer {
    WEB_PLATFORM_OBJECT(XRWebGLLayer, XRLayer);
    GC_DECLARE_ALLOCATOR(XRWebGLLayer);

public:
    using XRWebGLRenderingContext = Variant<GC::Root<WebGL::WebGLRenderingContext>, GC::Root<WebGL::WebGL2RenderingContext>>;

    [[nodiscard]] static GC::Ref<XRWebGLLayer> create(JS::Realm&);
    static WebIDL::ExceptionOr<GC::Ref<XRWebGLLayer>> construct_impl(JS::Realm&, XRSession const& session, XRWebGLRenderingContext const&, XRWebGLLayerInit const&);

private:
    XRWebGLLayer(JS::Realm&);
    virtual void visit_edges(JS::Cell::Visitor&) override;
};

}
