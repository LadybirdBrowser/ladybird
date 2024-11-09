/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>

namespace Web::PerformanceTimeline {

// https://w3c.github.io/performance-timeline/#dom-performanceobserverinit
struct PerformanceObserverInit {
    Optional<Vector<String>> entry_types;
    Optional<String> type;
    Optional<bool> buffered;
};

// https://w3c.github.io/performance-timeline/#dom-performanceobserver
class PerformanceObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PerformanceObserver, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(PerformanceObserver);

public:
    enum class ObserverType {
        Undefined,
        Single,
        Multiple,
    };

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<PerformanceObserver>> construct_impl(JS::Realm&, JS::GCPtr<WebIDL::CallbackType>);
    virtual ~PerformanceObserver() override;

    WebIDL::ExceptionOr<void> observe(PerformanceObserverInit& options);
    void disconnect();
    Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>> take_records();

    bool requires_dropped_entries() const { return m_requires_dropped_entries; }
    void unset_requires_dropped_entries(Badge<HTML::WindowOrWorkerGlobalScopeMixin>);

    Vector<PerformanceObserverInit> const& options_list() const { return m_options_list; }

    WebIDL::CallbackType& callback() { return *m_callback; }

    void append_to_observer_buffer(Badge<HTML::WindowOrWorkerGlobalScopeMixin>, JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>);

    static JS::NonnullGCPtr<JS::Object> supported_entry_types(JS::VM&);

private:
    PerformanceObserver(JS::Realm&, JS::GCPtr<WebIDL::CallbackType>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://w3c.github.io/performance-timeline/#dfn-observer-callback
    // A PerformanceObserverCallback observer callback set on creation.
    JS::GCPtr<WebIDL::CallbackType> m_callback;

    // https://w3c.github.io/performance-timeline/#dfn-observer-buffer
    // A PerformanceEntryList object called the observer buffer that is initially empty.
    Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>> m_observer_buffer;

    // https://w3c.github.io/performance-timeline/#dfn-observer-type
    // A DOMString observer type which is initially "undefined".
    ObserverType m_observer_type { ObserverType::Undefined };

    // https://w3c.github.io/performance-timeline/#dfn-requires-dropped-entries
    // A boolean requires dropped entries which is initially set to false.
    bool m_requires_dropped_entries { false };

    // https://w3c.github.io/performance-timeline/#dfn-options-list
    // A registered performance observer is a struct consisting of an observer member (a PerformanceObserver object)
    // and an options list member (a list of PerformanceObserverInit dictionaries).
    // NOTE: This doesn't use a separate struct as methods such as disconnect() assume it can access an options list from `this`: a PerformanceObserver.
    Vector<PerformanceObserverInit> m_options_list;
};

}
