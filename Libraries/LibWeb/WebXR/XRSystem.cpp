/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebXR/XRSystem.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRSystem);

GC::Ref<XRSystem> XRSystem::create(JS::Realm& realm)
{
    return realm.create<XRSystem>(realm);
}

XRSystem::XRSystem(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void XRSystem::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XRSystem);
    Base::initialize(realm);
}

void XRSystem::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://immersive-web.github.io/webxr/#dom-xrsystem-issessionsupported
GC::Ref<WebIDL::Promise> XRSystem::is_session_supported(Bindings::XRSessionMode mode) const
{
    // 1. Let promise be a new Promise in the relevant realm of this XRSystem.
    auto& realm = HTML::relevant_realm(*this);
    auto promise = WebIDL::create_promise(realm);

    // 2. If mode is "inline", resolve promise with true and return it.
    if (mode == Bindings::XRSessionMode::Inline) {
        WebIDL::resolve_promise(realm, promise, JS::Value(true));
        return promise;
    }

    // 3. If the requesting document’s origin is not allowed to use the "xr-spatial-tracking" permissions policy, reject promise with a "SecurityError" DOMException and return it.
    // FIXME: Implement this.

    // 4. Check whether the session mode is supported as follows:

    // -> If the user agent and system are known to never support mode sessions
    //    Resolve promise with false.
    WebIDL::resolve_promise(realm, promise, JS::Value(false));

    // -> If the user agent and system are known to usually support mode sessions
    //    promise MAY be resolved with true provided that all instances of this user agent
    //    indistinguishable by user agent string produce the same result here.
    // FIXME: Implement this.

    // -> Otherwise
    //    Run the following steps in parallel:
    // FIXME: We currently never end up here.
    //        Add all these steps once WebXR is more supported.

    // 5. Return promise.
    return promise;
}

}
