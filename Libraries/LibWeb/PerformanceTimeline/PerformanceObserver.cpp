/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/PerformanceObserver.h>
#include <LibWeb/Bindings/PerformanceObserverEntryList.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/SupportedPerformanceTypes.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserver.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserverEntryList.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::PerformanceTimeline {

GC_DEFINE_ALLOCATOR(PerformanceObserver);

static Bindings::WrapperWorldWeakValueCacheMap<JS::Object, JS::Object>& supported_entry_types_caches()
{
    static NeverDestroyed<Bindings::WrapperWorldWeakValueCacheMap<JS::Object, JS::Object>> caches;
    return *caches;
}

static Bindings::WrapperWorldWeakValueCache<JS::Object>& supported_entry_types_cache_for(JS::Object& global_object)
{
    return supported_entry_types_caches().cache_for(global_object);
}

static GC::Ref<JS::Object> create_supported_entry_types_array(JS::Realm& realm)
{
    auto& vm = realm.vm();
    GC::RootVector<JS::Value> supported_entry_types;

#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    supported_entry_types.append(JS::PrimitiveString::create(vm, entry_type));
    ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES

    auto supported_entry_types_array = JS::Array::create_from(realm, supported_entry_types);
    MUST(supported_entry_types_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    return supported_entry_types_array;
}

GC::Ref<PerformanceObserver> PerformanceObserver::create(GC::Ptr<WebIDL::CallbackType> callback, HTML::WindowOrWorkerGlobalScopeMixin& relevant_global)
{
    return GC::Heap::the().allocate<PerformanceObserver>(callback, relevant_global);
}

GC::Ref<PerformanceObserver> PerformanceObserver::create_for_constructor(JS::Realm& realm, GC::Ptr<WebIDL::CallbackType> callback)
{
    auto& relevant_global = HTML::relevant_window_or_worker_global_scope(realm.global_object());
    return create(callback, relevant_global);
}

PerformanceObserver::PerformanceObserver(GC::Ptr<WebIDL::CallbackType> callback, HTML::WindowOrWorkerGlobalScopeMixin& relevant_global)
    : m_callback(move(callback))
    , m_relevant_global(relevant_global.this_impl())
{
}

PerformanceObserver::~PerformanceObserver() = default;

void PerformanceObserver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_relevant_global);
    visitor.visit(m_observer_buffer);
}

HTML::WindowOrWorkerGlobalScopeMixin& PerformanceObserver::relevant_global() const
{
    return as<HTML::WindowOrWorkerGlobalScopeMixin>(*m_relevant_global);
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-observe
WebIDL::ExceptionOr<void> PerformanceObserver::observe(PerformanceObserverInit observer_options)
{
    // 1. Let relevantGlobal be this's relevant global object.
    auto& relevant_global = this->relevant_global();

    // 2. If options's entryTypes and type members are both omitted, then throw a "TypeError".
    if (!observer_options.entry_types.has_value() && !observer_options.type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Must specify one of entryTypes or type"sv };

    // 3. If options's entryTypes is present and any other member is also present, then throw a "TypeError".
    if (observer_options.entry_types.has_value() && (observer_options.type.has_value() || observer_options.buffered.has_value()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot specify type or buffered if entryTypes is specified"sv };

    // 4. Update or check this's observer type by running these steps:
    // 1. If this's observer type is "undefined":
    if (m_observer_type == ObserverType::Undefined) {
        // 1. If options's entryTypes member is present, then set this's observer type to "multiple".
        if (observer_options.entry_types.has_value())
            m_observer_type = ObserverType::Multiple;

        // 2. If options's type member is present, then set this's observer type to "single".
        if (observer_options.type.has_value())
            m_observer_type = ObserverType::Single;
    }
    // 2. If this's observer type is "single" and options's entryTypes member is present, then throw an "InvalidModificationError".
    else if (m_observer_type == ObserverType::Single) {
        if (observer_options.entry_types.has_value())
            return WebIDL::InvalidModificationError::create("Cannot change a PerformanceObserver from observing a single type to observing multiple types"_utf16);
    }
    // 3. If this's observer type is "multiple" and options's type member is present, then throw an "InvalidModificationError".
    else if (m_observer_type == ObserverType::Multiple) {
        if (observer_options.type.has_value())
            return WebIDL::InvalidModificationError::create("Cannot change a PerformanceObserver from observing multiple types to observing a single type"_utf16);
    }

    // 5. Set this's requires dropped entries to true.
    m_requires_dropped_entries = true;

    // 6. If this's observer type is "multiple", run the following steps:
    if (m_observer_type == ObserverType::Multiple) {
        // 1. Let entry types be options's entryTypes sequence.
        VERIFY(observer_options.entry_types.has_value());
        auto& entry_types = observer_options.entry_types.value();

        // 2. Remove all types from entry types that are not contained in relevantGlobal's frozen array of supported entry types.
        //    The user agent SHOULD notify developers if entry types is modified. For example, a console warning listing removed
        //    types might be appropriate.
        entry_types.remove_all_matching([](String const& type) {
#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    if (entry_type == type)                                                  \
        return false;
            ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES

            dbgln("Potential FIXME: Removing unsupported PerformanceEntry type '{}' from list of observed types in PerformanceObserver::observe()", type);
            return true;
        });

        // 3. If the resulting entry types sequence is an empty sequence, abort these steps.
        //    The user agent SHOULD notify developers when the steps are aborted to notify that registration has been aborted.
        //    For example, a console warning might be appropriate.
        if (entry_types.is_empty()) {
            dbgln("Potential FIXME: Returning from PerformanceObserver::observe() as we don't support any of the specified types (or none was specified).");
            return {};
        }

        // 4. If the list of registered performance observer objects of relevantGlobal contains a registered performance
        //    observer whose observer is this, replace its options list with a list containing options as its only item.
        // 5. Otherwise, create and append a registered performance observer object to the list of registered performance
        //    observer objects of relevantGlobal, with observer set to this and options list set to a list containing
        //    options as its only item.
        // NOTE: See the comment on PerformanceObserver::options_list about why this doesn't create a separate registered
        //       performance observer object.
        m_options_list.clear();
        m_options_list.append(observer_options);
        relevant_global.register_performance_observer({}, *this);
    }
    // 7. Otherwise, run the following steps:
    else {
        // 1. Assert that this's observer type is "single".
        VERIFY(m_observer_type == ObserverType::Single);

        // 2. If options's type is not contained in the relevantGlobal's frozen array of supported entry types, abort these steps.
        //    The user agent SHOULD notify developers when this happens, for instance via a console warning.
        VERIFY(observer_options.type.has_value());
        auto& type = observer_options.type.value();
        bool recognized_type = false;

#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    if (!recognized_type && entry_type == type)                              \
        recognized_type = true;
        ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES

        if (!recognized_type) {
            dbgln("Potential FIXME: Returning from PerformanceObserver::observe() as we don't support the PerformanceEntry type '{}'", type);
            return {};
        }

        // 3. If the list of registered performance observer objects of relevantGlobal contains a registered performance
        //    observer obs whose observer is this:
        if (relevant_global.has_registered_performance_observer(*this)) {
            // 1. If obs's options list contains a PerformanceObserverInit item currentOptions whose type is equal to options's type,
            //    replace currentOptions with options in obs's options list.
            auto index = m_options_list.find_first_index_if([&observer_options](PerformanceObserverInit const& entry) {
                return entry.type == observer_options.type;
            });
            if (index.has_value()) {
                m_options_list[index.value()] = observer_options;
            } else {
                // Otherwise, append options to obs's options list.
                m_options_list.append(observer_options);
            }
        }
        // 4. Otherwise, create and append a registered performance observer object to the list of registered performance
        //    observer objects of relevantGlobal, with observer set to the this and options list set to a list containing
        //    options as its only item.
        else {
            m_options_list.clear();
            m_options_list.append(observer_options);
            relevant_global.register_performance_observer({}, *this);
        }

        // 5. If options's buffered flag is set:
        if (observer_options.buffered.has_value() && observer_options.buffered.value()) {
            // 1. Let tuple be the relevant performance entry tuple of options's type and relevantGlobal.
            auto const& tuple = relevant_global.relevant_performance_entry_tuple(type);

            // 2. For each entry in tuple's performance entry buffer:
            for (auto const& entry : tuple.performance_entry_buffer) {
                // 1. If should add entry with entry and options as parameters returns true, append entry to the observer buffer.
                if (entry->should_add_entry(observer_options) == ShouldAddEntry::Yes)
                    m_observer_buffer.append(*entry);
            }

            // 3. Queue the PerformanceObserver task with relevantGlobal as input.
            relevant_global.queue_the_performance_observer_task();
        }
    }

    return {};
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-disconnect
void PerformanceObserver::disconnect()
{
    // 1. Remove this from the list of registered performance observer objects of relevant global object.
    auto& relevant_global = this->relevant_global();
    relevant_global.unregister_performance_observer({}, *this);

    // 2. Empty this's observer buffer.
    m_observer_buffer.clear();

    // 3. Empty this's options list.
    m_options_list.clear();
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-takerecords
Vector<GC::Root<PerformanceTimeline::PerformanceEntry>> PerformanceObserver::take_records()
{
    // The takeRecords() method must return a copy of this's observer buffer, and also empty this's observer buffer.
    Vector<GC::Root<PerformanceTimeline::PerformanceEntry>> records;
    for (auto& record : m_observer_buffer)
        records.append(*record);
    m_observer_buffer.clear();
    return records;
}

JS::Completion PerformanceObserver::invoke_callback(GC::Ref<PerformanceObserverEntryList> entry_list, Optional<u64> dropped_entries_count)
{
    auto& callback = this->callback();
    auto& realm = callback.callback_context->realm();
    auto callback_options = JS::Object::create(realm, realm.intrinsics().object_prototype());
    if (dropped_entries_count.has_value())
        MUST(callback_options->create_data_property("droppedEntriesCount"_utf16_fly_string, JS::Value(dropped_entries_count.value())));

    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto wrapped_observer_entry_list = Bindings::wrap(wrapper_world, realm, entry_list);
    auto wrapped_observer = Bindings::wrap(wrapper_world, realm, GC::Ref { *this });
    return WebIDL::invoke_callback(callback, wrapped_observer, { { wrapped_observer_entry_list, wrapped_observer, callback_options } });
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-supportedentrytypes
JS::Value PerformanceObserver::supported_entry_types(JS::Realm& realm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto& cache = supported_entry_types_cache_for(realm.global_object());

    if (auto supported_entry_types_array = cache.get(wrapper_world))
        return JS::Value(supported_entry_types_array);

    auto supported_entry_types_array = create_supported_entry_types_array(realm);
    cache.set(wrapper_world, supported_entry_types_array);
    return JS::Value(supported_entry_types_array);
}

void PerformanceObserver::unset_requires_dropped_entries(Badge<HTML::WindowOrWorkerGlobalScopeMixin>)
{
    m_requires_dropped_entries = false;
}

void PerformanceObserver::append_to_observer_buffer(Badge<HTML::WindowOrWorkerGlobalScopeMixin>, GC::Ref<PerformanceTimeline::PerformanceEntry> entry)
{
    m_observer_buffer.append(entry);
}

}
