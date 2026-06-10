/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/XRWebGLLayer.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebXR/XRLayer.h>
#include <LibWeb/WebXR/XRSession.h>

namespace Web::WebXR {

using XRWebGLLayerInit = Bindings::XRWebGLLayerInit;

// https://www.w3.org/TR/webxr/#xrwebgllayer-interface
class XRWebGLLayer : public XRLayer {
    WEB_WRAPPABLE(XRWebGLLayer, XRLayer);
    GC_DECLARE_ALLOCATOR(XRWebGLLayer);

public:
    using XRWebGLRenderingContext = Variant<GC::Ref<WebGL::WebGLRenderingContext>, GC::Ref<WebGL::WebGL2RenderingContext>>;

    [[nodiscard]] static GC::Ref<XRWebGLLayer> create();
    static WebIDL::ExceptionOr<GC::Ref<XRWebGLLayer>> create(XRSession const& session, XRWebGLRenderingContext const&, XRWebGLLayerInit const&);

private:
    XRWebGLLayer();
    virtual void visit_edges(JS::Cell::Visitor&) override;
};

}
