/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Bindings/DedicatedWorkerExposedInterfaces.h>
#include <LibWeb/Bindings/DedicatedWorkerGlobalScope.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/AnimationFrameCallbackDriver.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DedicatedWorkerGlobalScope);

DedicatedWorkerGlobalScope::DedicatedWorkerGlobalScope(JS::Realm& realm, GC::Ref<Web::Page> page)
    : WorkerGlobalScope(realm, page)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .has_global_interface_extended_attribute = true };
}

DedicatedWorkerGlobalScope::~DedicatedWorkerGlobalScope() = default;

void DedicatedWorkerGlobalScope::initialize_web_interfaces_impl()
{
    auto& realm = this->realm();
    add_dedicated_worker_exposed_interfaces(*this);

    DedicatedWorkerGlobalScopeGlobalMixin::initialize(realm, *this);

    Base::initialize_web_interfaces_impl();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-dedicatedworkerglobalscope-close
void DedicatedWorkerGlobalScope::close()
{
    // The close() method steps are to close a worker given this.
    close_a_worker();
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#concept-AnimationFrameProvider-supported
bool DedicatedWorkerGlobalScope::is_supported() const
{
    // An AnimationFrameProvider provider is considered supported if any of the following are true:
    for (auto const& owner : owner_set()) {
        // - provider is a Window.
        // NOTE: provider is this DedicatedWorkerGlobalScope.

        // - provider's owner set contains a Document object.
        if (owner.has<SerializedDocument>())
            return true;

        // - Any of the DedicatedWorkerGlobalScope objects in provider's owner set are supported.
        if (owner.get<SerializedWorkerGlobalScope>().is_supported_animation_frame_provider)
            return true;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-animationframeprovider-requestanimationframe
WebIDL::ExceptionOr<WebIDL::UnsignedLong> DedicatedWorkerGlobalScope::request_animation_frame(GC::Ref<WebIDL::CallbackType> callback)
{
    // FIXME: Make this fully spec compliant. Currently implements a mix of 'requestAnimationFrame()' and 'run the animation frame callbacks'.

    // 1. If this is not supported, then throw a "NotSupportedError" DOMException.
    if (!is_supported())
        return WebIDL::NotSupportedError::create(realm(), "requestAnimationFrame() is not supported in a dedicated worker whose owner set does not reach a Document"_utf16);

    // 2. Let target be this's target object.
    // NOTE: For a DedicatedWorkerGlobalScope, the target object is the DedicatedWorkerGlobalScope itself.

    // 3. Increment target's animation frame callback identifier by one, and let handle be the result.
    // 4. Let callbacks be target's map of animation frame callbacks.
    // 5. Set callbacks[handle] to callback.
    auto handle = animation_frame_callback_driver().add(GC::create_function(heap(), [this, callback](double now) {
        // Invoke callback with « now » and "report".
        auto result = WebIDL::invoke_callback(*callback, {}, { { JS::Value(now) } });
        if (result.is_error())
            report_exception(result, realm());
    }));
    schedule_rendering_update();

    // 6. Return handle.
    return handle;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animationframeprovider-cancelanimationframe
WebIDL::ExceptionOr<void> DedicatedWorkerGlobalScope::cancel_animation_frame(WebIDL::UnsignedLong handle)
{
    // 1. If this is not supported, then throw a "NotSupportedError" DOMException.
    if (!is_supported())
        return WebIDL::NotSupportedError::create(realm(), "cancelAnimationFrame() is not supported in a dedicated worker whose owner set does not reach a Document"_utf16);

    // 2. Let callbacks be this's target object's map of animation frame callbacks.
    // 3. Remove callbacks[handle].
    if (m_animation_frame_callback_driver)
        (void)m_animation_frame_callback_driver->remove(handle);
    return {};
}

AnimationFrameCallbackDriver& DedicatedWorkerGlobalScope::animation_frame_callback_driver()
{
    if (!m_animation_frame_callback_driver)
        m_animation_frame_callback_driver = realm().create<AnimationFrameCallbackDriver>();
    return *m_animation_frame_callback_driver;
}

bool DedicatedWorkerGlobalScope::has_animation_frame_callbacks() const
{
    return m_animation_frame_callback_driver && m_animation_frame_callback_driver->has_callbacks();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-loop-processing-model
// The processing model only updates a dedicated worker's rendering when "the user agent
// believes that it would benefit from having its rendering updated at this time", and notes
// that "a user agent can determine the rate of rendering in the dedicated worker". Worker
// event loops have no document-driven rendering, so a single-shot timer paced at the
// rendering rate of the spawning page (communicated when the worker was started) provides
// the rendering opportunities while animation frame callbacks or uncommitted
// OffscreenCanvas frames are pending.
// FIXME: The rate is fixed at worker start; propagate display rate changes to running workers.
void DedicatedWorkerGlobalScope::schedule_rendering_update()
{
    if (!m_rendering_update_timer) {
        m_rendering_update_timer = Platform::Timer::create_single_shot(heap(), 0, GC::create_function(heap(), [this] {
            run_rendering_update();
        }));
    }
    if (m_rendering_update_timer->is_active())
        return;
    auto interval_ms = static_cast<int>(AK::ceil(1000.0 / max(1.0, page()->client().maximum_frames_per_second())));
    m_rendering_update_timer->start(interval_ms);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-loop-processing-model
void DedicatedWorkerGlobalScope::run_rendering_update()
{
    // If this event loop's agent's single realm's global object is a supported
    // DedicatedWorkerGlobalScope and the user agent believes that it would benefit from having
    // its rendering updated at this time:

    // 1. Let now be the current high resolution time given the DedicatedWorkerGlobalScope.
    auto now = HighResolutionTime::current_high_resolution_time(*this);

    // 2. Run the animation frame callbacks for that DedicatedWorkerGlobalScope, passing in now
    //    as the timestamp.
    if (m_animation_frame_callback_driver)
        m_animation_frame_callback_driver->run(now);

    // 3. Update the rendering of that dedicated worker to reflect the current state.
    // NOTE: This presents the frames of placeholder-linked OffscreenCanvases that were drawn to.
    //       It runs whether or not animation frame callbacks were used, so canvases driven by
    //       timers or messages commit too.
    if (auto* page = this->page())
        page->prepare_canvas_contexts_for_compositing();

    if (has_animation_frame_callbacks())
        schedule_rendering_update();
}

void DedicatedWorkerGlobalScope::finalize()
{
    Base::finalize();
    WindowOrWorkerGlobalScopeMixin::finalize();
}

void DedicatedWorkerGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation_frame_callback_driver);
    visitor.visit(m_rendering_update_timer);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-dedicatedworkerglobalscope-postmessage-options
WebIDL::ExceptionOr<void> DedicatedWorkerGlobalScope::post_message(JS::Value message, Bindings::StructuredSerializeOptions const& options)
{
    // The postMessage(message, transfer) and postMessage(message, options) methods on DedicatedWorkerGlobalScope objects act as if,
    // when invoked, it immediately invoked the respective postMessage(message, transfer) and postMessage(message, options)
    // on the port, with the same arguments, and returned the same return value.
    return m_internal_port->post_message(message, options);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-dedicatedworkerglobalscope-postmessage
WebIDL::ExceptionOr<void> DedicatedWorkerGlobalScope::post_message(JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer)
{
    // The postMessage(message, transfer) and postMessage(message, options) methods on DedicatedWorkerGlobalScope objects act as if,
    // when invoked, it immediately invoked the respective postMessage(message, transfer) and postMessage(message, options)
    // on the port, with the same arguments, and returned the same return value.
    return m_internal_port->post_message(message, transfer);
}

WebIDL::CallbackType* DedicatedWorkerGlobalScope::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

void DedicatedWorkerGlobalScope::set_onmessage(WebIDL::CallbackType* callback)
{
    set_event_handler_attribute(EventNames::message, callback);

    // NOTE: This onmessage attribute setter implicitly sets worker's underlying MessagePort's onmessage attribute, so this
    //       spec behavior also applies here:
    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:handler-messageeventtarget-onmessage
    // The first time a MessagePort object's onmessage IDL attribute is set, the port's port message queue must be enabled,
    // as if the start() method had been called.
    m_internal_port->start();
}

void DedicatedWorkerGlobalScope::set_onmessageerror(WebIDL::CallbackType* callback)
{
    set_event_handler_attribute(EventNames::messageerror, callback);
}

WebIDL::CallbackType* DedicatedWorkerGlobalScope::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

}
