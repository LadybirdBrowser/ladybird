/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(CallbackType);

CallbackType::CallbackType(JS::Object& callback, HTML::EnvironmentSettingsObject& callback_context, OperationReturnsPromise operation_returns_promise)
    : callback(callback)
    , callback_context(callback_context)
    , operation_returns_promise(operation_returns_promise)
{
}

CallbackType::CallbackType(JS::Object& callback, JS::Realm& callback_context, OperationReturnsPromise operation_returns_promise)
    : CallbackType(callback, HTML::principal_realm_settings_object(callback_context), operation_returns_promise)
{
}

// https://webidl.spec.whatwg.org/#dfn-associated-realm
JS::Realm& CallbackType::associated_realm() const
{
    auto& realm = callback->shape().realm();
    if (callback->is_function())
        return realm;

    // NB: The specification leaves the realm of an object that is neither a platform object nor a function
    //     underspecified. Ours is the realm of its shape, which an object created with a cross-origin prototype takes
    //     from that prototype. So, matching other engines, it is kept only if it is same origin-domain with the
    //     callback context.
    // AD-HOC: This also applies to a cross-origin WindowProxy or Location itself, whose relevant realm the
    //         specification would enter. See https://github.com/whatwg/webidl/issues/1640.
    if (HTML::principal_realm_settings_object(realm).origin().is_same_origin_domain(callback_context->origin()))
        return realm;
    return callback_context->realm();
}

void CallbackType::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(callback_context);
}

}
