/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/XRRenderState.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#xrrenderstate
class XRRenderState : public Bindings::Wrappable {
    WEB_WRAPPABLE(XRRenderState, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(XRRenderState);

    XRRenderState();
};

}
