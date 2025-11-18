/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#dictdef-xrrenderstateinit
struct XRRenderStateInit {
    Optional<double> depth_near;
    Optional<double> depth_far;
    Optional<bool> passthrough_fully_obscured;
    Optional<double> inline_vertical_field_of_view;
};

// https://immersive-web.github.io/webxr/#xrrenderstate
class XRRenderState : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(XRRenderState, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(XRRenderState);

    XRRenderState(JS::Realm&);

    virtual void visit_edges(JS::Cell::Visitor&) override;
};

}
