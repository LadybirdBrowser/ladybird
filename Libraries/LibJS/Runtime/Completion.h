/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Try.h>
#include <AK/TypeCasts.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

#define TRY_OR_THROW_OOM(vm, ...)                                                                                     \
    ({                                                                                                                \
        /* Ignore -Wshadow to allow nesting the macro. */                                                             \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                                              \
            auto&& _temporary_result = (__VA_ARGS__));                                                                \
        if (_temporary_result.is_error()) {                                                                           \
            VERIFY(_temporary_result.error().code() == ENOMEM);                                                       \
            return (vm).throw_completion<JS::InternalError>((vm).error_message(::JS::VM::ErrorMessage::OutOfMemory)); \
        }                                                                                                             \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>,                  \
            "Do not return a reference from a fallible expression");                                                  \
        _temporary_result.release_value();                                                                            \
    })

#define MUST_OR_THROW_INTERNAL_ERROR(...)                                                            \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (__VA_ARGS__));                                               \
        if (_temporary_result.is_error()) {                                                          \
            auto _completion = _temporary_result.release_error();                                    \
                                                                                                     \
            VERIFY(_completion.value().has_value());                                                 \
            VERIFY(_completion.value()->is_object());                                                \
            VERIFY(::AK::is<JS::InternalError>(_completion.value()->as_object()));                   \
                                                                                                     \
            return _completion;                                                                      \
        }                                                                                            \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        _temporary_result.release_value();                                                           \
    })

// 6.2.3 The Completion Record Specification Type, https://tc39.es/ecma262/#sec-completion-record-specification-type
class [[nodiscard]] Completion {
public:
    enum class Type {
        Empty,
        Normal,
        Break,
        Continue,
        Return,
        Throw,
    };

    ALWAYS_INLINE Completion(Type type, Optional<Value> value)
        : m_type(type)
        , m_value(move(value))
    {
        VERIFY(type != Type::Empty);
        if (m_value.has_value())
            VERIFY(!m_value->is_empty());
    }

    Completion(ThrowCompletionOr<Value> const&);

    // 5.2.3.1 Implicit Completion Values, https://tc39.es/ecma262/#sec-implicit-completion-values
    // Not `explicit` on purpose.
    ALWAYS_INLINE Completion(Value value)
        : Completion(Type::Normal, value)
    {
    }

    ALWAYS_INLINE Completion(Optional<Value> value)
        : Completion(Type::Normal, move(value))
    {
    }

    ALWAYS_INLINE Completion()
        : Completion(js_undefined())
    {
    }

    Completion(Completion const&) = default;
    Completion& operator=(Completion const&) = default;
    Completion(Completion&&) = default;
    Completion& operator=(Completion&&) = default;

    [[nodiscard]] Type type() const
    {
        VERIFY(m_type != Type::Empty);
        return m_type;
    }
    [[nodiscard]] Optional<Value>& value() { return m_value; }
    [[nodiscard]] Optional<Value> const& value() const { return m_value; }

    // "abrupt completion refers to any completion with a [[Type]] value other than normal"
    [[nodiscard]] bool is_abrupt() const { return m_type != Type::Normal; }

    // These are for compatibility with the TRY() macro in AK.
    [[nodiscard]] bool is_error() const { return m_type == Type::Throw; }
    [[nodiscard]] Optional<Value> release_value() { return move(m_value); }
    Completion release_error()
    {
        VERIFY(is_error());
        VERIFY(m_value.has_value());
        return { m_type, release_value() };
    }

    // 6.2.3.4 UpdateEmpty ( completionRecord, value ), https://tc39.es/ecma262/#sec-updateempty
    Completion update_empty(Optional<Value> value) const
    {
        // 1. Assert: If completionRecord.[[Type]] is either return or throw, then completionRecord.[[Value]] is not empty.
        if (m_type == Type::Return || m_type == Type::Throw)
            VERIFY(m_value.has_value());

        // 2. If completionRecord.[[Value]] is not empty, return ? completionRecord.
        if (m_value.has_value())
            return *this;

        // 3. Return Completion Record { [[Type]]: completionRecord.[[Type]], [[Value]]: value, [[Target]]: completionRecord.[[Target]] }.
        return { m_type, move(value) };
    }

private:
    class EmptyTag {
    };
    friend AK::Optional<Completion>;

    Completion(EmptyTag)
        : m_type(Type::Empty)
    {
    }

    bool is_empty() const
    {
        return m_type == Type::Empty;
    }

    Type m_type { Type::Normal }; // [[Type]]
    Optional<Value> m_value;      // [[Value]]
    // NOTE: We don't need the [[Target]] slot since control flow is handled in bytecode.
};

}

namespace AK {

template<>
class Optional<JS::Completion> : public OptionalBase<JS::Completion> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = JS::Completion;

    Optional() = default;

    Optional(Optional<JS::Completion> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(move(other.m_value))
    {
    }

    template<typename U = JS::Completion>
    explicit(!IsConvertible<U&&, JS::Completion>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<JS::Completion>> && IsConstructible<JS::Completion, U &&>)
        : m_value(forward<U>(value))
    {
    }

    Optional& operator=(Optional const& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    void clear()
    {
        m_value = JS::Completion(JS::Completion::EmptyTag {});
    }

    [[nodiscard]] bool has_value() const
    {
        return !m_value.is_empty();
    }

    [[nodiscard]] JS::Completion& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Completion const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] JS::Completion value() &&
    {
        return release_value();
    }

    [[nodiscard]] JS::Completion release_value()
    {
        VERIFY(has_value());
        JS::Completion released_value = m_value;
        clear();
        return released_value;
    }

private:
    JS::Completion m_value { JS::Completion::EmptyTag {} };
};

}

namespace JS {

struct ErrorValue {
    Value error;
};

template<typename ValueType>
requires(!IsLvalueReference<ValueType>)
class [[nodiscard]] ThrowCompletionOr {
public:
    ALWAYS_INLINE ThrowCompletionOr()
    requires(IsSame<ValueType, Empty>)
        : m_value_or_error(Empty {})
    {
    }

    // Not `explicit` on purpose so that `return vm.throw_completion<Error>(...);` is possible.
    ALWAYS_INLINE ThrowCompletionOr(Completion throw_completion)
        : m_value_or_error(ErrorValue { throw_completion.release_error().value().value() })
    {
    }

    // Not `explicit` on purpose so that `return value;` is possible.
    ALWAYS_INLINE ThrowCompletionOr(ValueType value)
        : m_value_or_error(move(value))
    {
        if constexpr (IsSame<ValueType, Value>)
            VERIFY(!m_value_or_error.template get<ValueType>().is_empty());
    }

    ALWAYS_INLINE ThrowCompletionOr(ThrowCompletionOr const&) = default;
    ALWAYS_INLINE ThrowCompletionOr& operator=(ThrowCompletionOr const&) = default;
    ALWAYS_INLINE ThrowCompletionOr(ThrowCompletionOr&&) = default;
    ALWAYS_INLINE ThrowCompletionOr& operator=(ThrowCompletionOr&&) = default;

    ALWAYS_INLINE ThrowCompletionOr(OptionalNone value)
        : m_value_or_error(ValueType { value })
    {
    }

    // Allows implicit construction of ThrowCompletionOr<T> from a type U if T(U) is a supported constructor.
    // Most commonly: Value from Object* or similar, so we can omit the curly braces from "return { TRY(...) };".
    // Disabled for POD types to avoid weird conversion shenanigans.
    template<typename WrappedValueType>
    ALWAYS_INLINE ThrowCompletionOr(WrappedValueType&& value)
    requires(!IsPOD<ValueType>)
        : m_value_or_error(ValueType { value })
    {
    }

    [[nodiscard]] bool is_throw_completion() const { return m_value_or_error.template has<ErrorValue>(); }
    [[nodiscard]] Completion throw_completion() const { return error(); }
    [[nodiscard]] Value error_value() const { return m_value_or_error.template get<ErrorValue>().error; }

    [[nodiscard]] bool has_value() const
    requires(!IsSame<ValueType, Empty>)
    {
        return m_value_or_error.template has<ValueType>();
    }
    [[nodiscard]] ValueType const& value() const
    requires(!IsSame<ValueType, Empty>)
    {
        return m_value_or_error.template get<ValueType>();
    }

    // These are for compatibility with the TRY() macro in AK.
    [[nodiscard]] bool is_error() const { return m_value_or_error.template has<ErrorValue>(); }
    [[nodiscard]] ValueType release_value() { return move(m_value_or_error.template get<ValueType>()); }
    Completion error() const { return Completion { Completion::Type::Throw, m_value_or_error.template get<ErrorValue>().error }; }
    Completion release_error()
    {
        auto error = move(m_value_or_error.template get<ErrorValue>());
        return Completion { Completion::Type::Throw, error.error };
    }

    ValueType release_allocated_value_but_fixme_should_propagate_errors()
    {
        VERIFY(!is_error());
        return release_value();
    }

private:
    Variant<ValueType, ErrorValue> m_value_or_error;
};

template<>
class [[nodiscard]] ThrowCompletionOr<void> : public ThrowCompletionOr<Empty> {
public:
    using ThrowCompletionOr<Empty>::ThrowCompletionOr;
};

ThrowCompletionOr<Value> await(VM&, Value);

// 6.2.4.1 NormalCompletion ( value ), https://tc39.es/ecma262/#sec-normalcompletion
inline Completion normal_completion(Optional<Value> value)
{
    // 1. Return Completion Record { [[Type]]: normal, [[Value]]: value, [[Target]]: empty }.
    return { Completion::Type::Normal, move(value) };
}

// 6.2.4.2 ThrowCompletion ( value ), https://tc39.es/ecma262/#sec-throwcompletion
Completion throw_completion(Value);

}
