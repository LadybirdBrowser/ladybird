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
};

}
