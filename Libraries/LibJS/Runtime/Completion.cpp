/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static bool s_log_all_js_exceptions = false;

// Raw native functions, which JS_DEFINE_NATIVE_FUNCTION defines, return a ThrowCompletionOr<Value> straight to the
// Rust interpreter, which reads it as the embedding ABI's JSCompletion: the value or the thrown value in the first
// eight bytes, then the variant index, JS_COMPLETION_NORMAL for the value and JS_COMPLETION_THROW for the thrown value,
// as the alternatives are declared. The interpreter also expects it where this platform's C calling convention returns
// a trivially copyable struct of that size.
struct RawNativeFunctionResultLayout {
    using ValueOrError = decltype(ThrowCompletionOr<Value>::m_value_or_error);

    static_assert(sizeof(ThrowCompletionOr<Value>) == sizeof(JSCompletion));
    static_assert(alignof(ThrowCompletionOr<Value>) == alignof(JSCompletion));
    static_assert(IsTriviallyCopyable<ThrowCompletionOr<Value>>);
    static_assert(IsTriviallyDestructible<ThrowCompletionOr<Value>>);
    static_assert(sizeof(ValueOrError) == sizeof(ThrowCompletionOr<Value>));
    static_assert(IsSame<ValueOrError::IndexType, decltype(JSCompletion::variant)>);
    static_assert(sizeof(Value) == sizeof(JSCompletion::payload));
    static_assert(sizeof(ErrorValue) == sizeof(JSCompletion::payload));
};

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

// 6.2.4.2 ThrowCompletion ( value ), https://tc39.es/ecma262/#sec-throwcompletion
Completion throw_completion(Value value)
{
    if (s_log_all_js_exceptions)
        js_completion_log_exception(vm_to_abi(VM::the()), value_to_abi(value));

    // 1. Return Completion Record { [[Type]]: throw, [[Value]]: value, [[Target]]: empty }.
    return { Completion::Type::Throw, value };
}

static bool write_exception_log_line_with_dbgln(void*, u8 const* bytes, size_t length)
{
    dbgln("{}", StringView { bytes, length });
    return true;
}

// The runtime logs the exceptions it throws, and throw_completion() those that C++ code throws, both with dbgln().
void set_log_all_js_exceptions(bool enabled)
{
    s_log_all_js_exceptions = enabled;
    JSByteSink const exception_log_line_writer { nullptr, write_exception_log_line_with_dbgln };
    js_completion_set_log_all_exceptions(enabled, &exception_log_line_writer);
}

}
