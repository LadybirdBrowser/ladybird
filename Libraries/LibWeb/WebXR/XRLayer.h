/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#xrlayer
class XRLayer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(XRLayer, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(XRLayer);

    XRLayer(JS::Realm&);

    virtual void visit_edges(JS::Cell::Visitor&) override;
};

}
