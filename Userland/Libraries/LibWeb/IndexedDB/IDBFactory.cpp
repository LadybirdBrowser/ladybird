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
#include <LibWeb/IndexedDB/IDBFactory.h>
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

        (void)storage_key;
        (void)name;
        (void)version;
        (void)request;
    }));

    // 6. Return a new IDBOpenDBRequest object for request.
    return request;
}

}
