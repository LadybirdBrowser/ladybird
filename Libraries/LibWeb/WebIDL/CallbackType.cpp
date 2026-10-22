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

void CallbackType::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(callback_context);
}

}
