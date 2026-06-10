/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Bindings/XRSession.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebXR/XRSession.h>
#include <LibWeb/WebXR/XRSystem.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRSystem);

GC::Ref<XRSystem> XRSystem::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<XRSystem>(window);
}

XRSystem::XRSystem(HTML::Window& window)
    : DOM::EventTarget()
    , m_window(window)
{
}

void XRSystem::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_active_immersive_session);
    visitor.visit(m_list_of_inline_sessions);
    visitor.visit(m_window);
}

JS::Object& XRSystem::relevant_global_object() const
{
    return HTML::relevant_global_object(*m_window);
}

bool XRSystem::is_session_mode_supported(XRSessionMode mode) const
{
    if (mode == XRSessionMode::Inline)
        return true;

    // 3. If the requesting document’s origin is not allowed to use the "xr-spatial-tracking" permissions policy, reject promise with a "SecurityError" DOMException and return it.
    // FIXME: Implement this.

    // 4. Check whether the session mode is supported as follows:

    // -> If the user agent and system are known to usually support mode sessions
    //    promise MAY be resolved with true provided that all instances of this user agent
    //    indistinguishable by user agent string produce the same result here.
    // FIXME: Implement this.

    // -> Otherwise
    //    Run the following steps in parallel:
    // FIXME: We currently never end up here.
    //        Add all these steps once WebXR is more supported.

    // -> If the user agent and system are known to never support mode sessions
    //    Return false.
    return false;
}

void XRSystem::is_session_supported(JS::Realm& realm, XRSessionMode mode, GC::Ref<WebIDL::Promise> promise) const
{
    WebIDL::resolve_promise(realm, promise, JS::Value(is_session_mode_supported(mode)));
}

// https://immersive-web.github.io/webxr/#dom-xrsystem-requestsession
void XRSystem::request_session(JS::Realm& realm, XRSessionMode mode, XRSessionInit const& options, GC::Ref<WebIDL::Promise> promise)
{
    auto resolve = GC::create_function(GC::Heap::the(), [&realm, promise = GC::Root(promise)](GC::Ref<WebXR::XRSession> session) {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::resolve_promise(realm, *promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, session));
    });
    auto reject = GC::create_function(GC::Heap::the(), [&realm, promise = GC::Root(promise)](WebIDL::Exception exception) {
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::reject_promise_with_exception(realm, *promise, move(exception));
    });

    // 1. Let immersive be true if mode is an immersive session mode, and false otherwise.
    auto immersive = mode != XRSessionMode::Inline;

    // 2. Let global object be the relevant Global object for the XRSystem on which this method was invoked.
    auto& global_object = relevant_global_object();

    // 3. Check whether the session request is allowed as follows:

    // -> If immersive is true:
    if (immersive) {
        // 1. Check if an immersive session request is allowed for the global object, and if not
        //    reject promise with a "SecurityError" DOMException and return.
        //    FIXME: Implement this.
        (void)global_object;

        // 2. If pending immersive session is true or active immersive session is not null, reject
        //    promise with an "InvalidStateError" DOMException and return.
        if (m_pending_immersive_session || m_active_immersive_session) {
            reject->function()(WebIDL::InvalidStateError::create("An immersive session is already pending or active."_utf16));
            return;
        }

        // 3. Set pending immersive session to true.
        m_pending_immersive_session = true;
    }
    // -> Otherwise:
    else {
        // Check if an inline session request is allowed for the global object, and if not reject promise with
        // a "SecurityError" DOMException and return.
        // FIXME: Implement this.
    }

    // 4. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this, options, immersive, resolve, reject] {
        // 1. Let requiredFeatures be options’ requiredFeatures.
        auto required_features = options.required_features;

        // 2. Let optionalFeatures be options’ optionalFeatures.
        auto optional_features = options.optional_features;

        // 3. Set device to the result of obtaining the current device for mode, requiredFeatures, and optionalFeatures.
        // FIXME: Implement https://immersive-web.github.io/webxr/#obtain-the-current-device
        (void)required_features;
        (void)optional_features;

        // 4. Queue a task to perform the following steps:
        HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this, immersive, resolve, reject] {
            // 1. If device is null or device’s list of supported modes does not contain mode, run the following steps:
            // AD-HOC: Just reject immersive sessions here until we have devices.
            if (immersive) {
                // 1. Reject promise with a "NotSupportedError" DOMException.
                reject->function()(WebIDL::NotSupportedError::create("Sessions of this mode are not supported."_utf16));

                // 2. If immersive is true, set pending immersive session to false.
                if (immersive)
                    set_pending_immersive_session(false);

                // 3. Abort these steps.
                return;
            }

            // FIXME:
            // 2. Let descriptor be an XRPermissionDescriptor initialized with mode, requiredFeatures, and optionalFeatures
            // 3. Let status be an XRPermissionStatus, initially null
            // 4. Request the xr permission with descriptor and status.
            // 5. If status’ state is "denied" run the following steps:
            if (false) {
                // 1. Reject promise with a "NotSupportedError" DOMException.
                reject->function()(WebIDL::NotSupportedError::create("The XR Permissions are denied."_utf16));

                // 2. If immersive is true, set pending immersive session to false.
                if (immersive)
                    m_pending_immersive_session = false;

                // 3. Abort these steps.
                return;
            }

            // 6. Let granted be a set obtained from status’ granted.

            // 7. Let session be a new XRSession object in the relevant realm of this XRSystem.
            auto session = XRSession::create(*this);

            // 8. Initialize the session with session, mode, granted, and device.
            //    FIXME: Implement https://immersive-web.github.io/webxr/#initialize-the-session

            // 9. Potentially set the active immersive session as follows:

            // -> If immersive is true:
            if (immersive) {
                // Set the active immersive session to session, and set pending immersive session to false.
                m_active_immersive_session = session;
                m_pending_immersive_session = false;
            }
            // -> Otherwise:
            else {
                // Append session to the list of inline sessions.
                m_list_of_inline_sessions.append(session);
            }

            // 10. Resolve promise with session.
            resolve->function()(session);

            // 11. Queue a task to perform the following steps:
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(GC::Heap::the(), [session]() {
                // Note: These steps ensure that initial inputsourceschange events occur after the initial session is resolved.

                // 1. Set session’s promise resolved flag to true.
                session->set_promise_resolved(true);

                // 2. Let sources be any existing input sources attached to session.
                // FIXME: Implement this.

                // 3. If sources is non-empty, perform the following steps:
                if (false) {
                    // 1. Set session’s list of active XR input sources to sources.
                    // FIXME: Implement this.

                    // 2. Fire an XRInputSourcesChangeEvent named inputsourceschange on session with added set to sources.
                    // FIXME: Implement this.
                }
            }));
        }));
    }));
}

void XRSystem::remove_inline_session(GC::Ref<XRSession> session)
{
    m_list_of_inline_sessions.remove_first_matching([&](auto& entry) { return entry == session; });
}

}
