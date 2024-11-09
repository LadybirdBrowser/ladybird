/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceObserverPrototype.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/SupportedPerformanceTypes.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserver.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::PerformanceTimeline {

JS_DEFINE_ALLOCATOR(PerformanceObserver);

WebIDL::ExceptionOr<JS::NonnullGCPtr<PerformanceObserver>> PerformanceObserver::construct_impl(JS::Realm& realm, JS::GCPtr<WebIDL::CallbackType> callback)
{
    return realm.heap().allocate<PerformanceObserver>(realm, realm, callback);
}

PerformanceObserver::PerformanceObserver(JS::Realm& realm, JS::GCPtr<WebIDL::CallbackType> callback)
    : Bindings::PlatformObject(realm)
    , m_callback(move(callback))
{
}

PerformanceObserver::~PerformanceObserver() = default;

void PerformanceObserver::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceObserver);
}

void PerformanceObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_observer_buffer);
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-observe
WebIDL::ExceptionOr<void> PerformanceObserver::observe(PerformanceObserverInit& options)
{
    auto& realm = this->realm();

    // 1. Let relevantGlobal be this's relevant global object.
    auto* relevant_global = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&HTML::relevant_global_object(*this));
    VERIFY(relevant_global);

    // 2. If options's entryTypes and type members are both omitted, then throw a "TypeError".
    if (!options.entry_types.has_value() && !options.type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Must specify one of entryTypes or type"sv };

    // 3. If options's entryTypes is present and any other member is also present, then throw a "TypeError".
    if (options.entry_types.has_value() && (options.type.has_value() || options.buffered.has_value()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot specify type or buffered if entryTypes is specified"sv };

    // 4. Update or check this's observer type by running these steps:
    // 1. If this's observer type is "undefined":
    if (m_observer_type == ObserverType::Undefined) {
        // 1. If options's entryTypes member is present, then set this's observer type to "multiple".
        if (options.entry_types.has_value())
            m_observer_type = ObserverType::Multiple;

        // 2. If options's type member is present, then set this's observer type to "single".
        if (options.type.has_value())
            m_observer_type = ObserverType::Single;
    }
    // 2. If this's observer type is "single" and options's entryTypes member is present, then throw an "InvalidModificationError".
    else if (m_observer_type == ObserverType::Single) {
        if (options.entry_types.has_value())
            return WebIDL::InvalidModificationError::create(realm, "Cannot change a PerformanceObserver from observing a single type to observing multiple types"_string);
    }
    // 3. If this's observer type is "multiple" and options's type member is present, then throw an "InvalidModificationError".
    else if (m_observer_type == ObserverType::Multiple) {
        if (options.type.has_value())
            return WebIDL::InvalidModificationError::create(realm, "Cannot change a PerformanceObserver from observing multiple types to observing a single type"_string);
    }

    // 5. Set this's requires dropped entries to true.
    m_requires_dropped_entries = true;

    // 6. If this's observer type is "multiple", run the following steps:
    if (m_observer_type == ObserverType::Multiple) {
        // 1. Let entry types be options's entryTypes sequence.
        VERIFY(options.entry_types.has_value());
        auto& entry_types = options.entry_types.value();

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
        m_options_list.append(options);
        relevant_global->register_performance_observer({}, *this);
    }
    // 7. Otherwise, run the following steps:
    else {
        // 1. Assert that this's observer type is "single".
        VERIFY(m_observer_type == ObserverType::Single);

        // 2. If options's type is not contained in the relevantGlobal's frozen array of supported entry types, abort these steps.
        //    The user agent SHOULD notify developers when this happens, for instance via a console warning.
        VERIFY(options.type.has_value());
        auto& type = options.type.value();
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
        if (relevant_global->has_registered_performance_observer(*this)) {
            // 1. If obs's options list contains a PerformanceObserverInit item currentOptions whose type is equal to options's type,
            //    replace currentOptions with options in obs's options list.
            auto index = m_options_list.find_first_index_if([&options](PerformanceObserverInit const& entry) {
                return entry.type == options.type;
            });
            if (index.has_value()) {
                m_options_list[index.value()] = options;
            } else {
                // Otherwise, append options to obs's options list.
                m_options_list.append(options);
            }
        }
        // 4. Otherwise, create and append a registered performance observer object to the list of registered performance
        //    observer objects of relevantGlobal, with observer set to the this and options list set to a list containing
        //    options as its only item.
        else {
            m_options_list.clear();
            m_options_list.append(options);
            relevant_global->register_performance_observer({}, *this);
        }

        // 5. If options's buffered flag is set:
        if (options.buffered.has_value() && options.buffered.value()) {
            // 1. Let tuple be the relevant performance entry tuple of options's type and relevantGlobal.
            auto const& tuple = relevant_global->relevant_performance_entry_tuple(type);

            // 2. For each entry in tuple's performance entry buffer:
            for (auto const& entry : tuple.performance_entry_buffer) {
                // 1. If should add entry with entry and options as parameters returns true, append entry to the observer buffer.
                if (entry->should_add_entry(options) == ShouldAddEntry::Yes)
                    m_observer_buffer.append(*entry);
            }

            // 3. Queue the PerformanceObserver task with relevantGlobal as input.
            relevant_global->queue_the_performance_observer_task();
        }
    }

    return {};
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-disconnect
void PerformanceObserver::disconnect()
{
    // 1. Remove this from the list of registered performance observer objects of relevant global object.
    auto* relevant_global = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&HTML::relevant_global_object(*this));
    VERIFY(relevant_global);
    relevant_global->unregister_performance_observer({}, *this);

    // 2. Empty this's observer buffer.
    m_observer_buffer.clear();

    // 3. Empty this's options list.
    m_options_list.clear();
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-takerecords
Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>> PerformanceObserver::take_records()
{
    // The takeRecords() method must return a copy of this's observer buffer, and also empty this's observer buffer.
    Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>> records;
    for (auto& record : m_observer_buffer)
        records.append(*record);
    m_observer_buffer.clear();
    return records;
}

// https://w3c.github.io/performance-timeline/#dom-performanceobserver-supportedentrytypes
JS::NonnullGCPtr<JS::Object> PerformanceObserver::supported_entry_types(JS::VM& vm)
{
    // 1. Let globalObject be the environment settings object's global object.
    auto* window_or_worker = dynamic_cast<HTML::WindowOrWorkerGlobalScopeMixin*>(&vm.get_global_object());
    VERIFY(window_or_worker);

    // 2. Return globalObject's frozen array of supported entry types.
    return window_or_worker->supported_entry_types();
}

void PerformanceObserver::unset_requires_dropped_entries(Badge<HTML::WindowOrWorkerGlobalScopeMixin>)
{
    m_requires_dropped_entries = false;
}

void PerformanceObserver::append_to_observer_buffer(Badge<HTML::WindowOrWorkerGlobalScopeMixin>, JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry> entry)
{
    m_observer_buffer.append(entry);
}

}
