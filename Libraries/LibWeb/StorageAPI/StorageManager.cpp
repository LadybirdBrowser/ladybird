/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StorageManagerPrototype.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageManager);

WebIDL::ExceptionOr<GC::Ref<StorageManager>> StorageManager::create(JS::Realm& realm)
{
    return realm.create<StorageManager>(realm);
}

StorageManager::StorageManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void StorageManager::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StorageManager);
    Base::initialize(realm);
}

// https://storage.spec.whatwg.org/#queue-a-storage-task
void StorageManager::queue_a_storage_task(JS::Realm& realm, JS::Object& global, Function<void()> steps)
{
    HTML::queue_global_task(HTML::Task::Source::Storage, global, GC::create_function(realm.heap(), move(steps)));
}

// https://storage.spec.whatwg.org/#dom-storagemanager-estimate
GC::Ref<WebIDL::Promise> StorageManager::estimate() const
{
    // 1. Let promise be a new promise.
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);

    // 2. Let global be this’s relevant global object.
    auto& global = HTML::relevant_global_object(*this);

    // 3. Let shelf be the result of running obtain a local storage shelf with this’s relevant settings object.
    auto& settings = HTML::relevant_settings_object(*this);
    auto shelf = obtain_a_local_storage_shelf(settings);

    // 4. If shelf is failure, then reject promise with a TypeError.
    if (!shelf) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to obtain local storage shelf."_string));
    }
    // 5. Otherwise, run these steps in parallel:
    else {
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, &global, shelf = GC::Ref { *shelf }, promise] {
            // 1. Let usage be storage usage for shelf.
            auto usage = shelf->storage_usage();

            // 2. Let quota be storage quota for shelf.
            auto quota = shelf->storage_quota();

            // 3. Let dictionary be a new StorageEstimate dictionary whose usage member is usage and quota member is
            //    quota.
            auto dictionary_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
            dictionary_object->define_direct_property("usage"_utf16_fly_string, JS::Value(usage), JS::default_attributes);
            dictionary_object->define_direct_property("quota"_utf16_fly_string, JS::Value(quota), JS::default_attributes);
            auto dictionary_value = JS::Value { dictionary_object };

            // 4. If there was an internal error while obtaining usage and quota, then queue a storage task with global
            //    to reject promise with a TypeError.
            // There are no circumstances where an internal error can occur in our implementation, so we do nothing here.

            // 5. Otherwise, queue a storage task with global to resolve promise with dictionary.
            queue_a_storage_task(realm, global, [&realm, promise, dictionary_value] {
                HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                WebIDL::resolve_promise(realm, *promise, dictionary_value);
            });
        }));
    }

    // 6. Return promise.
    return promise;
}

// https://storage.spec.whatwg.org/#obtain-a-local-storage-shelf
GC::Ptr<StorageShelf> StorageManager::obtain_a_local_storage_shelf(HTML::EnvironmentSettingsObject& settings)
{
    // To obtain a local storage shelf, given an environment settings object environment, return the result of running
    // obtain a storage shelf with the user agent’s storage shed, environment, and "local".

    // FIXME: This should be implemented in a way that works for Workers.
    auto& window = as<HTML::Window>(settings.global_object());
    auto navigable = window.associated_document().navigable();
    VERIFY(navigable);
    VERIFY(navigable->traversable_navigable());
    auto& shed = navigable->traversable_navigable()->storage_shed();
    return shed.obtain_a_storage_shelf(settings, StorageType::Local);
}

}
