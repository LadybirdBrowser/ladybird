/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/IDBFactoryPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBFactory.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBFactory);

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
WebIDL::ExceptionOr<GC::Ref<IDBOpenDBRequest>> IDBFactory::open(String const& name, Optional<u64> version)
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
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, storage_key, name, version, request] {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. Let result be the result of opening a database connection, with storageKey, name, version if given and undefined otherwise, and request.
        auto result = open_a_database_connection(realm, storage_key.value(), name, version, request);

        // 2. Set request’s processed flag to true.
        request->set_processed(true);

        // 3. Queue a task to run these steps:
        HTML::queue_a_task(HTML::Task::Source::DatabaseAccess, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, request, result = move(result)]() mutable {
            // 1. If result is an error, then:
            if (result.is_error()) {
                // 1. Set request’s result to undefined.
                request->set_result(JS::js_undefined());

                // 2. Set request’s error to result.
                request->set_error(result.exception().get<GC::Ref<WebIDL::DOMException>>());

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

// https://w3c.github.io/IndexedDB/#dom-idbfactory-cmp
WebIDL::ExceptionOr<i8> IDBFactory::cmp(JS::Value first, JS::Value second)
{
    // 1. Let a be the result of converting a value to a key with first. Rethrow any exceptions.
    auto a = convert_a_value_to_a_key(realm(), first);

    // 2. If a is invalid, throw a "DataError" DOMException.
    if (a.is_error())
        return WebIDL::DataError::create(realm(), "Failed to convert a value to a key"_string);

    // 3. Let b be the result of converting a value to a key with second. Rethrow any exceptions.
    auto b = convert_a_value_to_a_key(realm(), second);

    // 4. If b is invalid, throw a "DataError" DOMException.
    if (b.is_error())
        return WebIDL::DataError::create(realm(), "Failed to convert a value to a key"_string);

    // 5. Return the results of comparing two keys with a and b.
    return Key::compare_two_keys(a.release_value(), b.release_value());
}

// https://w3c.github.io/IndexedDB/#dom-idbfactory-deletedatabase
WebIDL::ExceptionOr<GC::Ref<IDBOpenDBRequest>> IDBFactory::delete_database(String const& name)
{
    auto& realm = this->realm();

    // 1. Let environment be this's relevant settings object.
    auto& environment = HTML::relevant_settings_object(*this);

    // 2. Let storageKey be the result of running obtain a storage key given environment.
    //    If failure is returned, then throw a "SecurityError" DOMException and abort these steps.
    auto storage_key = StorageAPI::obtain_a_storage_key(environment);
    if (!storage_key.has_value())
        return WebIDL::SecurityError::create(realm, "Failed to obtain a storage key"_string);

    // 3. Let request be a new open request.
    auto request = IDBOpenDBRequest::create(realm);

    // 4. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, storage_key, name, request] {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. Let result be the result of deleting a database, with storageKey, name, and request.
        auto result = delete_a_database(realm, storage_key.value(), name, request);

        // 2. Set request’s processed flag to true.
        request->set_processed(true);

        // 3. Queue a task to run these steps:
        HTML::queue_a_task(HTML::Task::Source::DatabaseAccess, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, request, result = move(result)]() mutable {
            // 1.  If result is an error,
            if (result.is_error()) {
                // set request’s error to result,
                request->set_error(result.exception().get<GC::Ref<WebIDL::DOMException>>());
                // set request’s done flag to true,
                request->set_done(true);
                // and fire an event named error at request with its bubbles and cancelable attributes initialized to true.
                request->dispatch_event(DOM::Event::create(realm, HTML::EventNames::error, { .bubbles = true, .cancelable = true }));
            }
            // 2. Otherwise,
            else {
                // set request’s result to undefined,
                request->set_result(JS::js_undefined());
                // set request’s done flag to true,
                request->set_done(true);
                // and fire a version change event named success at request with result and null.
                auto value = result.release_value();
                fire_a_version_change_event(realm, HTML::EventNames::success, request, value, {});
            }
        }));
    }));

    // 5. Return a new IDBOpenDBRequest object for request.
    return request;
}

// https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
GC::Ref<WebIDL::Promise> IDBFactory::databases()
{
    auto& realm = this->realm();

    // 1. Let environment be this's relevant settings object.
    auto& environment = HTML::relevant_settings_object(*this);

    // 2. Let storageKey be the result of running obtain a storage key given environment.
    //    If failure is returned, then return a promise rejected with a "SecurityError" DOMException
    auto maybe_storage_key = StorageAPI::obtain_a_storage_key(environment);
    if (!maybe_storage_key.has_value())
        return WebIDL::create_rejected_promise_from_exception(realm, WebIDL::SecurityError::create(realm, "Failed to obtain a storage key"_string));

    auto storage_key = maybe_storage_key.release_value();

    // 3. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 4. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, storage_key, p]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. Let databases be the set of databases in storageKey.
        //    If this cannot be determined for any reason, then reject p with an appropriate error (e.g. an "UnknownError" DOMException) and terminate these steps.
        auto databases = Database::for_key(storage_key);

        // 2. Let result be a new list.
        auto result = MUST(JS::Array::create(realm, databases.size()));

        // 3. For each db of databases:
        for (u32 i = 0; i < databases.size(); ++i) {
            auto& db = databases[i];

            // 1. Let info be a new IDBDatabaseInfo dictionary.
            // 2. Set info’s name dictionary member to db’s name.
            // 3. Set info’s version dictionary member to db’s version.
            auto info = JS::Object::create(realm, realm.intrinsics().object_prototype());
            MUST(info->create_data_property("name"_fly_string, JS::PrimitiveString::create(realm.vm(), db->name())));
            MUST(info->create_data_property("version"_fly_string, JS::Value(db->version())));

            // 4. Append info to result.
            MUST(result->create_data_property_or_throw(i, info));
        }

        // 4. Resolve p with result.
        WebIDL::resolve_promise(realm, p, result);
    }));

    // 5. Return p.
    return p;
}

}
