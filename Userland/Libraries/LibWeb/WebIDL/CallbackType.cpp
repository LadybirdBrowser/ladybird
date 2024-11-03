/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Object.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::WebIDL {

JS_DEFINE_ALLOCATOR(CallbackType);

CallbackType::CallbackType(JS::Object& callback, HTML::EnvironmentSettingsObject& callback_context, OperationReturnsPromise operation_returns_promise)
    : callback(callback)
    , callback_context(callback_context)
    , operation_returns_promise(operation_returns_promise)
{
}

void CallbackType::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(callback_context);
}

}
