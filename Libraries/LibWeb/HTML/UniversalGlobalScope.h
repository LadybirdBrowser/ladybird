/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Utf16String.h>
#include <LibGC/HeapVector.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct StructuredSerializeOptions;

}

namespace Web::HTML {

// https://whatpr.org/html/9893/webappapis.html#universalglobalscope-mixin
class UniversalGlobalScopeMixin {
public:
    virtual ~UniversalGlobalScopeMixin();

    virtual DOM::EventTarget& this_impl() = 0;
    virtual DOM::EventTarget const& this_impl() const = 0;

    WebIDL::ExceptionOr<Utf16String> btoa(Utf16View data) const;
    WebIDL::ExceptionOr<Utf16String> atob(Utf16View data) const;
    void queue_microtask(WebIDL::CallbackType&);
    WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm&, JS::Value, Bindings::StructuredSerializeOptions const&);
    GC::Ref<WebIDL::CallbackType> count_queuing_strategy_size_function();
    GC::Ref<WebIDL::CallbackType> byte_length_queuing_strategy_size_function();

    void push_onto_outstanding_rejected_promises_weak_set(GC::Ptr<JS::Promise>);
    bool remove_from_outstanding_rejected_promises_weak_set(GC::Ptr<JS::Promise>);

    void push_onto_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise>);
    bool remove_from_about_to_be_notified_rejected_promises_list(GC::Ref<JS::Promise>);

    void notify_about_rejected_promises(Badge<EventLoop>);

    ImportMap& import_map() { return m_import_map; }
    ImportMap const& import_map() const { return m_import_map; }
    void set_import_map(ImportMap const& import_map) { m_import_map = import_map; }

    static WEB_API void set_experimental_interfaces_exposed(bool);
    static WEB_API bool expose_experimental_interfaces();

protected:
    void visit_edges(GC::Cell::Visitor&);

private:
    GC::Ptr<WebIDL::CallbackType> m_count_queuing_strategy_size_function;
    GC::Ptr<WebIDL::CallbackType> m_byte_length_queuing_strategy_size_function;
    GC::Ptr<GC::HeapVector<GC::Ref<JS::Promise>>> m_about_to_be_notified_rejected_promises_list;
    Vector<GC::Ptr<JS::Promise>> m_outstanding_rejected_promises_weak_set;
    ImportMap m_import_map;
};

}
