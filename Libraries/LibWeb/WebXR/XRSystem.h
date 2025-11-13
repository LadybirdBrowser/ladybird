/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/XRSystemPrototype.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#xrsystem-interface
class XRSystem final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(XRSystem, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(XRSystem);

public:
    static GC::Ref<XRSystem> create(JS::Realm&);
    virtual ~XRSystem() override = default;

    // https://immersive-web.github.io/webxr/#dom-xrsystem-issessionsupported
    GC::Ref<WebIDL::Promise> is_session_supported(Bindings::XRSessionMode) const;

private:
    XRSystem(JS::Realm&);
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(JS::Cell::Visitor&) override;
};

}
