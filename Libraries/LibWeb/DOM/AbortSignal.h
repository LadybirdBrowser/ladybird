/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#abortsignal
class AbortSignal final : public EventTarget {
    WEB_PLATFORM_OBJECT(AbortSignal, EventTarget);
    JS_DECLARE_ALLOCATOR(AbortSignal);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> construct_impl(JS::Realm&);

    virtual ~AbortSignal() override = default;

    void add_abort_algorithm(ESCAPING Function<void()>);

    // https://dom.spec.whatwg.org/#dom-abortsignal-aborted
    // An AbortSignal object is aborted when its abort reason is not undefined.
    bool aborted() const { return !m_abort_reason.is_undefined(); }

    void signal_abort(JS::Value reason);

    void set_onabort(WebIDL::CallbackType*);
    WebIDL::CallbackType* onabort();

    // https://dom.spec.whatwg.org/#dom-abortsignal-reason
    JS::Value reason() const { return m_abort_reason; }
    void set_reason(JS::Value reason) { m_abort_reason = reason; }

    JS::ThrowCompletionOr<void> throw_if_aborted() const;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> abort(JS::VM&, JS::Value reason);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> timeout(JS::VM&, Web::WebIDL::UnsignedLongLong milliseconds);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> any(JS::VM&, Vector<JS::Handle<AbortSignal>> const&);

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AbortSignal>> create_dependent_abort_signal(JS::Realm&, Vector<JS::Handle<AbortSignal>> const&);

private:
    explicit AbortSignal(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    bool dependent() const { return m_dependent; }
    void set_dependent(bool dependent) { m_dependent = dependent; }

    Vector<JS::GCPtr<AbortSignal>> source_signals() const { return m_source_signals; }

    void append_source_signal(JS::GCPtr<AbortSignal> source_signal) { m_source_signals.append(source_signal); }
    void append_dependent_signal(JS::GCPtr<AbortSignal> dependent_signal) { m_dependent_signals.append(dependent_signal); }

    // https://dom.spec.whatwg.org/#abortsignal-abort-reason
    // An AbortSignal object has an associated abort reason, which is a JavaScript value. It is undefined unless specified otherwise.
    JS::Value m_abort_reason { JS::js_undefined() };

    // https://dom.spec.whatwg.org/#abortsignal-abort-algorithms
    // FIXME: This should be a set.
    Vector<JS::NonnullGCPtr<JS::HeapFunction<void()>>> m_abort_algorithms;

    // https://dom.spec.whatwg.org/#abortsignal-source-signals
    // An AbortSignal object has associated source signals (a weak set of AbortSignal objects that the object is dependent on for its aborted state), which is initially empty.
    Vector<JS::GCPtr<AbortSignal>> m_source_signals;

    // https://dom.spec.whatwg.org/#abortsignal-dependent-signals
    // An AbortSignal object has associated dependent signals (a weak set of AbortSignal objects that are dependent on the object for their aborted state), which is initially empty.
    Vector<JS::GCPtr<AbortSignal>> m_dependent_signals;

    // https://dom.spec.whatwg.org/#abortsignal-dependent
    // An AbortSignal object has a dependent (a boolean), which is initially false.
    bool m_dependent { false };
};

}
