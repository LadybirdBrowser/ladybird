/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBFactoryPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBFactory.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBFactory);

IDBFactory::IDBFactory(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

IDBFactory::~IDBFactory() = default;

void IDBFactory::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBFactory);
}

// https://w3c.github.io/IndexedDB/#dom-idbfactory-open
WebIDL::ExceptionOr<JS::NonnullGCPtr<IDBOpenDBRequest>> IDBFactory::open(String const& name, Optional<u64> version)
{
    auto& realm = this->realm();

    // 1. If version is 0 (zero), throw a TypeError.
    if (version.has_value() && version.value() == 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The version provided must not be 0"_string };

    // 2. Let environment be this's relevant settings object.
    auto& environment = HTML::relevant_settings_object(*this);

    // 3. Let storageKey be the result of running obtain a storage key given environment.
    //    If failure is returned, then throw a "SecurityError" DOMException and abort these steps.
    auto storage_key = StorageAPI::obtain_a_storage_key(environment);
    if (!storage_key.has_value())
        return WebIDL::SecurityError::create(realm, "Failed to obtain a storage key"_string);

    // 4. Let request be a new open request.
    auto request = IDBOpenDBRequest::create(realm);

    // 5. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(JS::create_heap_function(realm.heap(), [&realm, storage_key, name, version, request] {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. Let result be the result of opening a database connection, with storageKey, name, version if given and undefined otherwise, and request.
        auto result = open_a_database_connection(realm, storage_key.value(), name, version, request);

        // 2. Queue a task to run these steps:
        HTML::queue_a_task(HTML::Task::Source::DatabaseAccess, nullptr, nullptr, JS::create_heap_function(realm.heap(), [&realm, &request, result = move(result)]() mutable {
            // 1. If result is an error, then:
            if (result.is_error()) {
                // 1. Set request’s result to undefined.
                request->set_result(JS::js_undefined());

                // 2. Set request’s error to result.
                request->set_error(result.exception().get<JS::NonnullGCPtr<WebIDL::DOMException>>());

                // 3. Set request’s done flag to true.
                request->set_done(true);

                // 4. Fire an event named error at request with its bubbles and cancelable attributes initialized to true.
                request->dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));
            } else {
                // 1. Set request’s result to result.
                request->set_result(result.release_value());

                // 2. Set request’s done flag to true.
                request->set_done(true);

                // 3. Fire an event named success at request.
                request->dispatch_event(DOM::Event::create(realm, HTML::EventNames::success));
            }
        }));
    }));

    // 6. Return a new IDBOpenDBRequest object for request.
    return request;
}

}
