/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Agent.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

bool g_log_all_js_exceptions = false;

Completion::Completion(ThrowCompletionOr<Value> const& throw_completion_or_value)
{
    if (throw_completion_or_value.is_throw_completion()) {
        m_type = Type::Throw;
        m_value = throw_completion_or_value.throw_completion().value();
    } else {
        m_type = Type::Normal;
        m_value = throw_completion_or_value.value();
    }
}

// 6.2.3.1 Await, https://tc39.es/ecma262/#await
// FIXME: Replace this with the bytecode implementation (e.g. by converting users to bytecode)
ThrowCompletionOr<Value> await(VM& vm, Value)
{
    return vm.throw_completion<InternalError>(ErrorType::NotImplemented, "Migrating old await implementation to Bytecode"sv);
}

static void log_exception(Value value)
{
    if (!value.is_object()) {
        dbgln("\033[31;1mTHROW!\033[0m {}", value);
        return;
    }

    auto& object = value.as_object();
    auto& vm = object.vm();
    dbgln("\033[31;1mTHROW!\033[0m {}", object.get(vm.names.message).value());
    vm.dump_backtrace();
}

// 6.2.4.2 ThrowCompletion ( value ), https://tc39.es/ecma262/#sec-throwcompletion
Completion throw_completion(Value value)
{
    if (g_log_all_js_exceptions)
        log_exception(value);

    // 1. Return Completion Record { [[Type]]: throw, [[Value]]: value, [[Target]]: empty }.
    return { Completion::Type::Throw, value };
}

void set_log_all_js_exceptions(bool const enabled)
{
    g_log_all_js_exceptions = enabled;
}

}
