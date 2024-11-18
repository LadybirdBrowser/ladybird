/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/String.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://whatpr.org/html/9893/webappapis.html#universalglobalscope-mixin
class UniversalGlobalScopeMixin {
public:
    virtual ~UniversalGlobalScopeMixin();

    virtual Bindings::PlatformObject& this_impl() = 0;
    virtual Bindings::PlatformObject const& this_impl() const = 0;

    WebIDL::ExceptionOr<String> btoa(String const& data) const;
    WebIDL::ExceptionOr<String> atob(String const& data) const;
    void queue_microtask(WebIDL::CallbackType&);
    WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Value, StructuredSerializeOptions const&) const;

    GC::Ref<WebIDL::CallbackType> count_queuing_strategy_size_function();
    GC::Ref<WebIDL::CallbackType> byte_length_queuing_strategy_size_function();

protected:
    void visit_edges(GC::Cell::Visitor&);

private:
    // https://streams.spec.whatwg.org/#count-queuing-strategy-size-function
    GC::Ptr<WebIDL::CallbackType> m_count_queuing_strategy_size_function;

    // https://streams.spec.whatwg.org/#byte-length-queuing-strategy-size-function
    GC::Ptr<WebIDL::CallbackType> m_byte_length_queuing_strategy_size_function;
};

}
