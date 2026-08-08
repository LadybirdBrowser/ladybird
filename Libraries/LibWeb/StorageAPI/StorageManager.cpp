/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/StorageAPI/StorageManager.h>
#include <LibWeb/StorageAPI/StorageShelf.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageManager);

GC::Ref<StorageManager> StorageManager::create(GC::Ref<HTML::EnvironmentSettingsObject> environment)
{
    return GC::Heap::the().allocate<StorageManager>(environment);
}

StorageManager::StorageManager(GC::Ref<HTML::EnvironmentSettingsObject> environment)
    : m_environment(environment)
{
}

void StorageManager::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_environment);
}

void StorageManager::queue_a_storage_task(JS::Object& global, Function<void()> steps)
{
    HTML::queue_global_task(HTML::Task::Source::Storage, global, GC::create_function(GC::Heap::the(), move(steps)));
}

GC::Ref<WebIDL::Promise> StorageManager::estimate() const
{
    auto& realm = m_environment->realm();
    auto promise = WebIDL::create_promise_for(m_environment->global_object());
    auto& global = m_environment->global_object();
    auto shelf = obtain_a_local_storage_shelf(*m_environment);

    if (!shelf) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Failed to obtain local storage shelf."_utf16));
    } else {
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, &global, shelf = GC::Ref { *shelf }, promise] {
            auto usage = shelf->storage_usage();
            auto quota = shelf->storage_quota();
            auto dictionary_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
            dictionary_object->define_direct_property("usage"_utf16_fly_string, JS::Value(usage), JS::default_attributes);
            dictionary_object->define_direct_property("quota"_utf16_fly_string, JS::Value(quota), JS::default_attributes);
            auto dictionary_value = JS::Value { dictionary_object };
            queue_a_storage_task(global, [&realm, promise, dictionary_value] {
                HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                WebIDL::resolve_promise(*promise, dictionary_value);
            });
        }));
    }

    return promise;
}

}
