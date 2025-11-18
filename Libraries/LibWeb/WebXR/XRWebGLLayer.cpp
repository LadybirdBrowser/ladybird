/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/XRWebGLLayerPrototype.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>
#include <LibWeb/WebXR/XRWebGLLayer.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRWebGLLayer);

XRWebGLLayer::XRWebGLLayer(JS::Realm& realm)
    : XRLayer(realm)
{
}

GC::Ref<XRWebGLLayer> XRWebGLLayer::create(JS::Realm& realm)
{
    return realm.create<XRWebGLLayer>(realm);
}

// https://immersive-web.github.io/webxr/#dom-xrwebgllayer-xrwebgllayer
WebIDL::ExceptionOr<GC::Ref<XRWebGLLayer>> XRWebGLLayer::construct_impl(JS::Realm& realm, XRSession const& session, XRWebGLRenderingContext const& context, XRWebGLLayerInit const& layer_init)
{
    // 1. Let layer be a new XRWebGLLayer in the relevant realm of session.
    auto layer = create(realm);

    // 2. If session’s ended value is true, throw an InvalidStateError and abort these steps.
    if (session.ended())
        return WebIDL::InvalidStateError::create(realm, "The XRSession has ended."_utf16);

    // 3. If context is lost, throw an InvalidStateError and abort these steps.
    auto* contextBase = context.visit([](auto p) { return static_cast<WebGL::WebGLRenderingContextBase*>(&*p); });
    if (contextBase->is_context_lost())
        return WebIDL::InvalidStateError::create(realm, "The context has been lost."_utf16);

    // 4. If session is an immersive session and context’s XR compatible boolean is false, throw an InvalidStateError
    //    and abort these steps.
    if (session.is_immersive() && !contextBase->xr_compatible())
        return WebIDL::InvalidStateError::create(realm, "The XRSession is an immersive one, but the context is not XR-compatible."_utf16);

    // FIXME: Implement all of these.
    (void)layer_init;
    // 5. Initialize layer’s context to context.
    // 6. Initialize layer’s session to session.
    // 7. Initialize layer’s ignoreDepthValues as follows:
    // 8. Initialize layer’s composition enabled boolean as follows:
    // -> If session is an inline session:
    //     Initialize layer’s composition enabled to false.
    // -> Otherwise:
    //     Initialize layer’s composition enabled boolean to true.
    // 9.-> If layer’s composition enabled boolean is true:
    //     1. Initialize layer’s antialias to layerInit’s antialias value.
    //     2. Let scaleFactor be layerInit’s framebufferScaleFactor.
    //     3. The user agent MAY choose to clamp or round scaleFactor as it sees fit here, for example if it wishes to fit
    //        the buffer dimensions into a power of two for performance reasons.
    //     4. Let framebufferSize be the recommended WebGL framebuffer resolution with width and height separately
    //        multiplied by scaleFactor.
    //     5. Initialize layer’s framebuffer to a new WebGLFramebuffer in the relevant realm of context, which is an opaque
    //        framebuffer with the dimensions framebufferSize created with context, session initialized to session, and
    //        layerInit’s depth, stencil, and alpha values.
    //     6. Allocate and initialize resources compatible with session’s XR device, including GPU accessible memory
    //        buffers, as required to support the compositing of layer.
    //     7. If layer’s resources were unable to be created for any reason, throw an OperationError and abort these steps.
    // Otherwise:
    //     1. Initialize layer’s antialias to layer’s context’s actual context parameters antialias value.
    //     2. Initialize layer’s framebuffer to null.

    return layer;
}

void XRWebGLLayer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
