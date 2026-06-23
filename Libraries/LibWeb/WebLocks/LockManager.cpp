/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/NeverDestroyed.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/LockManager.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebLocks/Lock.h>
#include <LibWeb/WebLocks/LockManager.h>
#include <LibWeb/WebLocks/LockRequest.h>

namespace Web::WebLocks {

GC_DEFINE_ALLOCATOR(LockManager);

GC::Ref<LockManager> LockManager::create(JS::Realm& realm)
{
    return realm.create<LockManager>(realm);
}

LockManager::LockManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void LockManager::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(LockManager);
    Base::initialize(realm);
}

void LockManager::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_lock_request_queue_map);
    visitor.visit(m_held_lock_set);
}

// https://w3c.github.io/web-locks/#dom-lockmanager-request
GC::Ref<WebIDL::Promise> LockManager::request(String const& name, GC::Ref<WebIDL::CallbackType> callback)
{
    // 1. If options was not passed, then let options be a new LockOptions dictionary with default members.
    return request(name, {}, callback);
}

// https://w3c.github.io/web-locks/#dom-lockmanager-request
GC::Ref<WebIDL::Promise> LockManager::request(String const& name, Bindings::LockOptions const& options, GC::Ref<WebIDL::CallbackType> callback)
{
    auto& realm = this->realm();

    // 2. Let environment be this’s relevant settings object.
    auto& environment = HTML::relevant_settings_object(*this);

    // 3. If environment’s relevant global object’s associated Document is not fully active, then return a promise
    //    rejected with a "InvalidStateError" DOMException.
    if (auto* window = as_if<HTML::Window>(environment.global_object())) {
        if (!window->associated_document().is_fully_active())
            return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16));
    }

    // 4. Let manager be the result of obtaining a lock manager given environment. If that returned failure, then return
    //    a promise rejected with a "SecurityError" DOMException.
    auto& manager = obtain_lock_manager(environment);

    // 5. If name starts with U+002D HYPHEN-MINUS (-), then return a promise rejected with a "NotSupportedError"
    //    DOMException.
    if (name.starts_with('-'))
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::NotSupportedError::create(realm, "Lock names starting with '-' are reserved"_utf16));

    // 6. If both options["steal"] and options["ifAvailable"] are true, then return a promise rejected with a
    //    "NotSupportedError" DOMException.
    if (options.steal && options.if_available)
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::NotSupportedError::create(realm, "Lock options 'steal' and 'ifAvailable' cannot both be true"_utf16));

    // 7. If options["steal"] is true and options["mode"] is not "exclusive", then return a promise rejected with a
    //    "NotSupportedError" DOMException.
    if (options.steal && options.mode != Bindings::LockMode::Exclusive)
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::NotSupportedError::create(realm, "Lock option 'steal' can only be used with exclusive locks"_utf16));

    // 8. If options["signal"] exists, and either of options["steal"] or options["ifAvailable"] is true, then return a
    //    promise rejected with a "NotSupportedError" DOMException.
    if (options.signal && (options.steal || options.if_available))
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::NotSupportedError::create(realm, "Lock option 'signal' cannot be combined with 'steal' or 'ifAvailable'"_utf16));

    // 9. If options["signal"] exists and is aborted, then return a promise rejected with options["signal"]'s abort reason.
    if (options.signal && options.signal->aborted())
        return WebIDL::create_rejected_promise(realm, options.signal->reason());

    // 10. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 11. Request a lock with promise, the current agent, environment’s id, manager, callback, name, options["mode"],
    //     options["ifAvailable"], options["steal"], and options["signal"].
    manager.request_lock(promise, callback, name, options.mode, options.if_available, options.steal, options.signal);

    // 12. Return promise.
    return promise;
}

// https://w3c.github.io/web-locks/#dom-lockmanager-query
GC::Ref<WebIDL::Promise> LockManager::query()
{
    auto& realm = this->realm();

    // 1. Let environment be this’s relevant settings object.
    auto& environment = HTML::relevant_settings_object(*this);

    // 2. If environment’s relevant global object’s associated Document is not fully active, then return a promise
    //    rejected with a "InvalidStateError" DOMException.
    if (auto* window = as_if<HTML::Window>(environment.global_object())) {
        if (!window->associated_document().is_fully_active())
            return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16));
    }

    // 3. Let manager be the result of obtaining a lock manager given environment. If that returned failure, then return
    //    a promise rejected with a "SecurityError" DOMException.
    auto& manager = obtain_lock_manager(environment);

    // 4. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 5. Enqueue the steps to snapshot the lock state for manager with promise to the lock task queue.
    queue_web_locks_task(realm, GC::create_function(heap(), [manager = GC::Ref { manager }, promise]() {
        manager->snapshot_lock_state(promise);
    }));

    // 6. Return promise.
    return promise;
}

// https://w3c.github.io/web-locks/#request-a-lock
GC::Ref<LockRequest> LockManager::request_lock(GC::Ref<WebIDL::Promise> promise, GC::Ref<WebIDL::CallbackType> callback, String name, Bindings::LockMode mode, bool if_available, bool steal, GC::Ptr<DOM::AbortSignal> signal)
{
    auto& environment = HTML::relevant_settings_object(*this);

    // 1. Let request be a new lock request (agent, clientId, manager, name, mode, callback, promise, signal).
    auto request = heap().allocate<LockRequest>(environment.id, *this, move(name), mode, callback, promise, signal);

    // 2. If signal is present, then add the algorithm signal to abort the request request with signal to signal.
    if (signal) {
        request->set_abort_algorithm_id(signal->add_abort_algorithm([request, signal]() {
            request->signal_to_abort_request(*signal);
        }));
    }

    // 3. Enqueue the following steps to the lock task queue:
    queue_web_locks_task(realm(), GC::create_function(heap(), [this, request, if_available, steal]() {
        auto& realm = this->realm();

        // 1. Let queueMap be manager’s lock request queue map.
        // 2. Let queue be the result of getting the lock request queue from queueMap for name.
        auto& queue = get_lock_request_queue(m_lock_request_queue_map, request->name());

        // 3. Let held be manager’s held lock set.
        // 4. If steal is true, then run these steps:
        if (steal) {
            // 1. For each lock of held:
            for (size_t i = 0; i < m_held_lock_set.size();) {
                auto lock = m_held_lock_set[i];

                // 1. If lock’s name is name, then run these steps:
                if (lock->name() == request->name()) {
                    // 1. Remove lock from held.
                    m_held_lock_set.remove(i);

                    // 2. Reject lock’s released promise with an "AbortError" DOMException.
                    WebIDL::reject_promise(realm, lock->released_promise(), WebIDL::AbortError::create(realm, "Lock was stolen"_utf16));
                } else {
                    ++i;
                }
            }

            // 2. Prepend request in queue.
            queue.prepend(request);
        }
        // 5. Otherwise, run these steps:
        else {
            // 1. If ifAvailable is true and request is not grantable, then enqueue the following steps on callback’s
            //    relevant settings object’s responsible event loop:
            if (if_available && !request->is_grantable(queue)) {
                queue_web_locks_task_on_relevant_event_loop(realm, request->callback(), GC::create_function(heap(), [this, request]() {
                    auto& realm = this->realm();

                    // 1. Let r be the result of invoking callback with null as the only argument.
                    auto result = WebIDL::invoke_promise_callback(request->callback(), {}, { { JS::js_null() } });

                    // 2. Resolve promise with r and abort these steps.
                    WebIDL::resolve_promise(realm, request->promise(), result->promise());
                }));

                // NB: This request has completed with no lock. Continuing here would enqueue it and allow it to be
                //     granted later, causing callback to be invoked a second time with a Lock.
                return;
            }

            // 2. Enqueue request in queue.
            queue.append(request);
        }

        // 6. Process the lock request queue queue.
        process_lock_request_queue(queue);
    }));

    // 4. Return request.
    return request;
}

// https://w3c.github.io/web-locks/#process-the-lock-request-queue
void LockManager::process_lock_request_queue(LockRequestQueue& queue)
{
    auto& realm = this->realm();

    // 1. Assert: these steps are running on the lock task queue.

    // 2. For each request of queue:
    for (size_t i = 0; i < queue.size();) {
        auto request = queue[i];

        // 1. If request is not grantable, then return.
        // NOTE: Only the first item in a queue is grantable. Therefore, if something is not grantable then all the
        //       following items are automatically not grantable.
        if (!request->is_grantable(queue))
            return;

        // 2. Remove request from queue.
        queue.remove(i);

        // 3. Let agent be request’s agent.

        // 4. Let manager be request’s manager.
        auto& manager = request->manager();

        // 5. Let clientId be request’s clientId.
        auto const& client_id = request->client_id();

        // 6. Let name be request’s name.
        auto const& name = request->name();

        // 7. Let name be request’s name.
        auto mode = request->mode();

        // 8. Let callback be request’s callback.
        auto callback = request->callback();

        // 9. Let p be request’s promise.
        auto promise = request->promise();

        // 10. Let signal be request’s signal.
        auto signal = request->signal();

        // 11. Let waiting be a new promise.
        auto waiting = WebIDL::create_promise(realm);

        // 12. Let lock be a new lock with agent agent, clientId clientId, manager manager, mode mode, name name,
        //     released promise p, and waiting promise waiting.
        auto lock = heap().allocate<LockData>(client_id, manager, mode, name, promise, waiting);

        // 13. Append lock to manager’s held lock set.
        manager.m_held_lock_set.append(lock);

        // 14. Enqueue the following steps on callback’s relevant settings object’s responsible event loop:
        queue_web_locks_task_on_relevant_event_loop(realm, callback, GC::create_function(heap(), [this, request, lock, signal, callback, waiting]() {
            auto& realm = this->realm();

            // 1. If signal is present, then run these steps:
            if (signal) {
                // 1. If signal is aborted, then run these steps:
                if (signal->aborted()) {
                    // 1. Enqueue the following step to the lock task queue:
                    queue_web_locks_task(realm, GC::create_function(heap(), [lock]() {
                        // 1. Release the lock lock.
                        lock->release_lock();
                    }));

                    // 2. Return.
                    return;
                }

                // 2. Remove the algorithm signal to abort the request request from signal.
                if (auto abort_algorithm_id = request->abort_algorithm_id(); abort_algorithm_id.has_value())
                    signal->remove_abort_algorithm(*abort_algorithm_id);
            }

            // 2. Let r be the result of invoking callback with a new Lock object associated with lock as the only argument.
            auto result = WebIDL::invoke_promise_callback(callback, {}, { { Lock::create(realm, lock) } });

            // 3. Resolve waiting with r.
            WebIDL::resolve_promise(realm, waiting, result->promise());
        }));

        // https://w3c.github.io/web-locks/#waiting-promise-settles
        // When lock's waiting promise settles (fulfills or rejects), enqueue the following steps on the lock task queue:
        auto waiting_promise_settled_steps = GC::create_function(heap(), [this, lock](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            auto& realm = this->realm();

            queue_web_locks_task(realm, GC::create_function(heap(), [this, lock]() {
                auto& realm = this->realm();

                // 1. Release the lock lock.
                lock->release_lock();

                // 2. Resolve lock's released promise with lock's waiting promise.
                WebIDL::resolve_promise(realm, lock->released_promise(), lock->waiting_promise()->promise());
            }));

            return JS::js_undefined();
        });

        WebIDL::react_to_promise(waiting, waiting_promise_settled_steps, waiting_promise_settled_steps);
    }
}

// https://w3c.github.io/web-locks/#snapshot-the-lock-state
void LockManager::snapshot_lock_state(GC::Ref<WebIDL::Promise> promise)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    static NeverDestroyed<AK::Utf16FlyString> CLIENT_ID = "clientId"_utf16_fly_string;
    static NeverDestroyed<AK::Utf16FlyString> HELD = "held"_utf16_fly_string;
    static NeverDestroyed<AK::Utf16FlyString> MODE = "mode"_utf16_fly_string;
    static NeverDestroyed<AK::Utf16FlyString> NAME = "name"_utf16_fly_string;
    static NeverDestroyed<AK::Utf16FlyString> PENDING = "pending"_utf16_fly_string;

    auto create_lock_info_object = [&](auto lock) {
        auto lock_info = JS::Object::create(realm, realm.intrinsics().object_prototype());
        MUST(lock_info->create_data_property_or_throw(*NAME, JS::PrimitiveString::create(vm, Utf16String::from_utf8(lock->name()))));
        MUST(lock_info->create_data_property_or_throw(*MODE, JS::PrimitiveString::create(vm, Bindings::idl_enum_to_string(lock->mode()))));
        MUST(lock_info->create_data_property_or_throw(*CLIENT_ID, JS::PrimitiveString::create(vm, Utf16String::from_utf8(lock->client_id()))));

        return lock_info;
    };

    // 1. Assert: these steps are running on the lock task queue.

    // 2. Let pending be a new list.
    auto pending = MUST(JS::Array::create(realm, 0));
    u32 pending_index = 0;

    // 3. For each queue of manager’s lock request queue map’s values:
    for (auto const& queue : m_lock_request_queue_map) {
        // 1. For each request of queue:
        for (auto request : queue.value) {
            // 1. Append «[ "name" → request’s name, "mode" → request’s mode, "clientId" → request’s clientId ]» to pending.
            MUST(pending->create_data_property_or_throw(pending_index++, create_lock_info_object(request)));
        }
    }

    // 4. Let held be a new list.
    auto held = MUST(JS::Array::create(realm, 0));

    // 5. For each lock of manager’s held lock set:
    for (auto [i, lock] : enumerate(m_held_lock_set)) {
        // 1. Append «[ "name" → lock’s name, "mode" → lock’s mode, "clientId" → lock’s clientId ]» to held.
        MUST(held->create_data_property_or_throw(i, create_lock_info_object(lock)));
    }

    // 6. Resolve promise with «[ "held" → held, "pending" → pending ]».
    auto snapshot = JS::Object::create(realm, realm.intrinsics().object_prototype());
    MUST(snapshot->create_data_property_or_throw(*HELD, held));
    MUST(snapshot->create_data_property_or_throw(*PENDING, pending));
    WebIDL::resolve_promise(realm, promise, snapshot);
}

}
