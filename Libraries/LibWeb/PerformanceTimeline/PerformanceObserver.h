/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::PerformanceTimeline {

class PerformanceObserverEntryList;

// https://w3c.github.io/performance-timeline/#dom-performanceobserver
class PerformanceObserver final : public Bindings::Wrappable {
    WEB_WRAPPABLE(PerformanceObserver, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(PerformanceObserver);

public:
    enum class ObserverType {
        Undefined,
        Single,
        Multiple,
    };

    static GC::Ref<PerformanceObserver> create(GC::Ptr<WebIDL::CallbackType>, HTML::WindowOrWorkerGlobalScopeMixin&);
    static GC::Ref<PerformanceObserver> create_for_constructor(JS::Realm&, GC::Ptr<WebIDL::CallbackType>);
    virtual ~PerformanceObserver() override;

    WebIDL::ExceptionOr<void> observe(PerformanceObserverInit options);
    void disconnect();
    Vector<GC::Root<PerformanceTimeline::PerformanceEntry>> take_records();

    bool requires_dropped_entries() const { return m_requires_dropped_entries; }
    void unset_requires_dropped_entries(Badge<HTML::WindowOrWorkerGlobalScopeMixin>);

    Vector<PerformanceObserverInit> const& options_list() const { return m_options_list; }
    WebIDL::CallbackType& callback() { return *m_callback; }
    JS::Completion invoke_callback(GC::Ref<PerformanceObserverEntryList>, Optional<u64> dropped_entries_count);
    static JS::Value supported_entry_types(JS::Realm&);

    void append_to_observer_buffer(Badge<HTML::WindowOrWorkerGlobalScopeMixin>, GC::Ref<PerformanceTimeline::PerformanceEntry>);

private:
    PerformanceObserver(GC::Ptr<WebIDL::CallbackType>, HTML::WindowOrWorkerGlobalScopeMixin&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    HTML::WindowOrWorkerGlobalScopeMixin& relevant_global() const;

    // https://w3c.github.io/performance-timeline/#dfn-observer-callback
    // A PerformanceObserverCallback observer callback set on creation.
    GC::Ptr<WebIDL::CallbackType> m_callback;

    GC::Ref<DOM::EventTarget> m_relevant_global;

    // https://w3c.github.io/performance-timeline/#dfn-observer-buffer
    // A PerformanceEntryList object called the observer buffer that is initially empty.
    Vector<GC::Ref<PerformanceTimeline::PerformanceEntry>> m_observer_buffer;

    // https://w3c.github.io/performance-timeline/#dfn-observer-type
    // A DOMString observer type which is initially "undefined".
    ObserverType m_observer_type { ObserverType::Undefined };

    // https://w3c.github.io/performance-timeline/#dfn-requires-dropped-entries
    // A boolean requires dropped entries which is initially set to false.
    bool m_requires_dropped_entries { false };

    // https://w3c.github.io/performance-timeline/#dfn-options-list
    // A registered performance observer is a struct consisting of an observer member (a PerformanceObserver object)
    // and an options list member (a list of PerformanceObserverInit objects).
    // NOTE: This doesn't use a separate struct as methods such as disconnect() assume it can access an options list from `this`: a PerformanceObserver.
    Vector<PerformanceObserverInit> m_options_list;
};

}
