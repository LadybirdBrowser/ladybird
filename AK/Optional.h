/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <AK/Try.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {

namespace Detail {

template<auto condition, typename T>
struct ConditionallyResultType;

template<typename T>
struct ConditionallyResultType<true, T> {
    using Type = typename T::ResultType;
};

template<typename T>
struct ConditionallyResultType<false, T> {
    using Type = T;
};

}

template<auto condition, typename T>
using ConditionallyResultType = typename Detail::ConditionallyResultType<condition, T>::Type;

// NOTE: If you're here because of an internal compiler error in GCC 10.3.0+,
//       it's because of the following bug:
//
//       https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96745
//
//       Make sure you didn't accidentally make your destructor private before
//       you start bug hunting. :^)

template<typename>
class Optional;

struct OptionalNone {
    explicit constexpr OptionalNone() = default;
};

template<typename T, typename Self = Optional<T>>
requires(!IsLvalueReference<Self>) class [[nodiscard]] OptionalBase {
public:
    using ValueType = T;

    template<SameAs<OptionalNone> V>
    ALWAYS_INLINE constexpr Self& operator=(V)
    {
        static_cast<Self&>(*this).clear();
        return static_cast<Self&>(*this);
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T* ptr() &
    {
        return static_cast<Self&>(*this).has_value() ? __builtin_launder(reinterpret_cast<T*>(&static_cast<Self&>(*this).value())) : nullptr;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const* ptr() const&
    {
        return static_cast<Self const&>(*this).has_value() ? __builtin_launder(reinterpret_cast<T const*>(&static_cast<Self const&>(*this).value())) : nullptr;
    }

    template<typename O = T, typename Fallback = O>
    [[nodiscard]] ALWAYS_INLINE constexpr O value_or(Fallback const& fallback) const&
    {
        if (static_cast<Self const&>(*this).has_value())
            return static_cast<Self const&>(*this).value();
        return fallback;
    }

    template<typename O = T, typename Fallback = O>
    requires(!IsLvalueReference<O> && !IsRvalueReference<O>)
    [[nodiscard]] ALWAYS_INLINE constexpr O value_or(Fallback&& fallback) &&
    {
        if (static_cast<Self&>(*this).has_value())
            return move(static_cast<Self&>(*this).value());
        return move(fallback);
    }

    template<typename Callback, typename O = T>
    [[nodiscard]] ALWAYS_INLINE constexpr O value_or_lazy_evaluated(Callback callback) const
    {
        if (static_cast<Self const&>(*this).has_value())
            return static_cast<Self const&>(*this).value();
        return callback();
    }

    template<typename Callback, typename O = T>
    [[nodiscard]] ALWAYS_INLINE constexpr Optional<O> value_or_lazy_evaluated_optional(Callback callback) const
    {
        if (static_cast<Self const&>(*this).has_value())
            return static_cast<Self const&>(*this).value();
        return callback();
    }

    template<typename Callback, typename O = T>
    [[nodiscard]] ALWAYS_INLINE constexpr ErrorOr<O> try_value_or_lazy_evaluated(Callback callback) const
    {
        if (static_cast<Self const&>(*this).has_value())
            return static_cast<Self const&>(*this).value();
        return TRY(callback());
    }

    template<typename Callback, typename O = T>
    [[nodiscard]] ALWAYS_INLINE constexpr ErrorOr<Optional<O>> try_value_or_lazy_evaluated_optional(Callback callback) const
    {
        if (static_cast<Self const&>(*this).has_value())
            return static_cast<Self const&>(*this).value();
        return TRY(callback());
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const& operator*() const { return static_cast<Self const&>(*this).value(); }
    [[nodiscard]] ALWAYS_INLINE constexpr T& operator*() { return static_cast<Self&>(*this).value(); }

    ALWAYS_INLINE constexpr T const* operator->() const { return &static_cast<Self const&>(*this).value(); }
    ALWAYS_INLINE constexpr T* operator->() { return &static_cast<Self&>(*this).value(); }

    template<typename F, typename MappedType = decltype(declval<F>()(declval<T&>())), auto IsErrorOr = IsSpecializationOf<MappedType, ErrorOr>, typename OptionalType = Optional<ConditionallyResultType<IsErrorOr, MappedType>>>
    ALWAYS_INLINE constexpr Conditional<IsErrorOr, ErrorOr<OptionalType>, OptionalType> map(F&& mapper)
    {
        if constexpr (IsErrorOr) {
            if (static_cast<Self&>(*this).has_value())
                return OptionalType { TRY(mapper(static_cast<Self&>(*this).value())) };
            return OptionalType {};
        } else {
            if (static_cast<Self&>(*this).has_value())
                return OptionalType { mapper(static_cast<Self&>(*this).value()) };

            return OptionalType {};
        }
    }

    template<typename F, typename MappedType = decltype(declval<F>()(declval<T&>())), auto IsErrorOr = IsSpecializationOf<MappedType, ErrorOr>, typename OptionalType = Optional<ConditionallyResultType<IsErrorOr, MappedType>>>
    ALWAYS_INLINE constexpr Conditional<IsErrorOr, ErrorOr<OptionalType>, OptionalType> map(F&& mapper) const
    {
        if constexpr (IsErrorOr) {
            if (static_cast<Self const&>(*this).has_value())
                return OptionalType { TRY(mapper(static_cast<Self const&>(*this).value())) };
            return OptionalType {};
        } else {
            if (static_cast<Self const&>(*this).has_value())
                return OptionalType { mapper(static_cast<Self const&>(*this).value()) };

            return OptionalType {};
        }
    }
};

template<typename T>
requires(!IsLvalueReference<T>) class [[nodiscard]] Optional<T> : public OptionalBase<T, Optional<T>> {
    template<typename U>
    friend class Optional;

    static_assert(!IsLvalueReference<T> && !IsRvalueReference<T>);

public:
    using ValueType = T;

    ALWAYS_INLINE constexpr Optional()
    {
        construct_null_if_necessary();
    }

    template<SameAs<OptionalNone> V>
    ALWAYS_INLINE constexpr Optional(V)
    {
        construct_null_if_necessary();
    }

    template<SameAs<OptionalNone> V>
    ALWAYS_INLINE constexpr Optional& operator=(V)
    {
        clear();
        return *this;
    }

    AK_MAKE_CONDITIONALLY_COPYABLE(Optional, <T>);
    AK_MAKE_CONDITIONALLY_MOVABLE(Optional, <T>);
    AK_MAKE_CONDITIONALLY_DESTRUCTIBLE(Optional, <T>);

    ALWAYS_INLINE constexpr Optional(Optional const& other)
    requires(!IsTriviallyCopyConstructible<T>)
        : m_has_value(other.m_has_value)
    {
        if (other.has_value())
            construct_at<RemoveConst<T>>(&m_storage, other.value());
        else
            construct_null_if_necessary();
    }

    ALWAYS_INLINE constexpr Optional(Optional&& other)
    requires(!IsTriviallyMoveConstructible<T>)
        : m_has_value(other.m_has_value)
    {
        if (other.has_value())
            construct_at<RemoveConst<T>>(&m_storage, other.release_value());
        else
            construct_null_if_necessary();
    }

    template<typename U>
    requires(IsConstructible<T, U const&> && !IsSpecializationOf<T, Optional> && !IsSpecializationOf<U, Optional> && (!IsLvalueReference<U> || IsTriviallyCopyConstructible<U>)) ALWAYS_INLINE explicit constexpr Optional(Optional<U> const& other)
        : m_has_value(other.has_value())
    {
        if (other.has_value())
            construct_at<RemoveConst<T>>(&m_storage, other.value());
        else
            construct_null_if_necessary();
    }

    template<typename U>
    requires(IsConstructible<T, U &&> && !IsSpecializationOf<T, Optional> && !IsSpecializationOf<U, Optional> && (!IsLvalueReference<U> || IsTriviallyMoveConstructible<U>)) ALWAYS_INLINE explicit constexpr Optional(Optional<U>&& other)
        : m_has_value(other.has_value())
    {
        if (other.has_value())
            construct_at<RemoveConst<T>>(&m_storage, other.release_value());
        else
            construct_null_if_necessary();
    }

    template<typename U = T>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    ALWAYS_INLINE explicit(!IsConvertible<U&&, T>) constexpr Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<T>> && IsConstructible<T, U &&>)
        : m_has_value(true)
    {
        construct_at<RemoveConst<T>>(&m_storage, forward<U>(value));
    }

    ALWAYS_INLINE constexpr Optional& operator=(Optional const& other)
    requires(!IsTriviallyCopyConstructible<T> || !IsTriviallyDestructible<T>)
    {
        if (this != &other) {
            clear();
            m_has_value = other.m_has_value;
            if (other.has_value())
                construct_at<RemoveConst<T>>(&m_storage, other.value());
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    requires(!IsMoveConstructible<T> || !IsDestructible<T>)
    = delete;

    // Note: This overload is optional. It exists purely to match the SerenityOS and `std::optional` behaviour.
    // The only (observable) difference between this overload and the next one is that this one calls the move assignment operator when both `this` and `other` have a value.
    // The other overload just unconditionally calls the move constructor.
    ALWAYS_INLINE constexpr Optional& operator=(Optional&& other)
    requires(IsMoveAssignable<T> && IsMoveConstructible<T> && (!IsTriviallyMoveAssignable<T> || !IsTriviallyMoveConstructible<T> || !IsTriviallyDestructible<T>))
    {
        if (this != &other) {
            if (has_value() && other.has_value()) {
                value() = other.release_value();
            } else if (has_value()) {
                value().~T();
                m_has_value = false;
            } else if (other.has_value()) {
                m_has_value = true;
                construct_at<RemoveConst<T>>(&m_storage, other.release_value());
            }
        }
        return *this;
    }

    // Allow for move constructible but non-move assignable types, such as those containing const or reference fields,
    // Note: This overload can also handle move assignable types perfectly fine, but the behaviour would be slightly different.
    ALWAYS_INLINE constexpr Optional& operator=(Optional&& other)
    requires(!IsMoveAssignable<T> && IsMoveConstructible<T> && (!IsTriviallyMoveConstructible<T> || !IsTriviallyDestructible<T>))
    {
        if (this != &other) {
            clear();
            m_has_value = other.m_has_value;
            if (other.has_value())
                construct_at<RemoveConst<T>>(&m_storage, other.release_value());
        }
        return *this;
    }

    template<class U = T>
    requires(!IsOneOfIgnoringCVReference<U, Optional<T>, OptionalNone> && !(IsSame<U, T> && IsScalar<U>))
    // Note: We restrict this to `!IsScalar<U>` to prevent undesired overload resolution for `= {}`.
    ALWAYS_INLINE constexpr Optional<T>& operator=(U&& value)
    requires(IsConstructible<T, U &&>)
    {
        if constexpr (IsAssignable<AddLvalueReference<T>, AddRvalueReference<U>>) {
            if (m_has_value)
                m_storage = forward<U>(value);
            else
                construct_at<RemoveConst<T>>(&m_storage, forward<U>(value));
            m_has_value = true;
        } else {
            emplace(forward<U>(value));
        }
        return *this;
    }

    ALWAYS_INLINE constexpr ~Optional()
    requires(!IsTriviallyDestructible<T> && IsDestructible<T>)
    {
        clear();
    }

    ALWAYS_INLINE constexpr void clear()
    {
        if (m_has_value) {
            value().~T();
            m_has_value = false;
        }
    }

    template<typename... Parameters>
    ALWAYS_INLINE constexpr void emplace(Parameters&&... parameters)
    {
        clear();
        m_has_value = true;
        construct_at<RemoveConst<T>>(&m_storage, forward<Parameters>(parameters)...);
    }

    template<typename Callable>
    ALWAYS_INLINE constexpr void lazy_emplace(Callable callable)
    {
        clear();
        m_has_value = true;
        construct_at<RemoveConst<T>>(&m_storage, callable());
    }

    [[nodiscard]] ALWAYS_INLINE constexpr bool has_value() const { return m_has_value; }

    [[nodiscard]] ALWAYS_INLINE constexpr T& value() &
    {
        VERIFY(m_has_value);
        return m_storage;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const& value() const&
    {
        VERIFY(m_has_value);
        return m_storage;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T value() &&
    {
        return release_value();
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T release_value()
    {
        VERIFY(m_has_value);
        T released_value = move(value());
        value().~T();
        m_has_value = false;
        return released_value;
    }

private:
    ALWAYS_INLINE constexpr void construct_null_if_necessary(bool should_construct = is_constant_evaluated())
    {
        // OPTIMIZATION: Only construct the `m_null` member when we are constant-evaluating.
        // Otherwise, this generates an unnecessary zero-fill.
#if defined(AK_COMPILER_GCC)
        // NOTE: GCCs -Wuninitialized warning ends up checking this as well.
        should_construct = true;
#endif
        if (should_construct)
            construct_at(&m_null);
    }

    union {
        // FIXME: GCC seems to have an issue with uninitialized unions and non trivial types,
        //        which forces us to have an equally sized trivial null member in the union
        //        to pseudo-initialize the union.
        struct {
            u8 _[sizeof(T)];
        } m_null;
        RemoveConst<T> m_storage;
    };
    bool m_has_value { false };
};

template<typename T>
requires(IsLvalueReference<T>) class [[nodiscard]] Optional<T> {
    AK_MAKE_DEFAULT_COPYABLE(Optional);
    AK_MAKE_DEFAULT_MOVABLE(Optional);

    template<typename>
    friend class Optional;

    template<typename U>
    constexpr static bool CanBePlacedInOptional = IsSame<RemoveReference<T>, RemoveReference<AddConstToReferencedType<U>>> && (IsBaseOf<RemoveCVReference<T>, RemoveCVReference<U>> || IsSame<RemoveCVReference<T>, RemoveCVReference<U>>);

public:
    using ValueType = T;

    ALWAYS_INLINE constexpr Optional() = default;

    template<SameAs<OptionalNone> V>
    ALWAYS_INLINE constexpr Optional(V) { }

    template<SameAs<OptionalNone> V>
    ALWAYS_INLINE constexpr Optional& operator=(V)
    {
        clear();
        return *this;
    }

    template<typename U = T>
    ALWAYS_INLINE constexpr Optional(U& value)
    requires(CanBePlacedInOptional<U&>)
        : m_pointer(&value)
    {
    }

    ALWAYS_INLINE constexpr Optional(RemoveReference<T>& value)
        : m_pointer(&value)
    {
    }

    template<typename U>
    ALWAYS_INLINE constexpr Optional(Optional<U>& other)
    requires(CanBePlacedInOptional<U>)
        : m_pointer(other.ptr())
    {
    }

    template<typename U>
    ALWAYS_INLINE constexpr Optional(Optional<U> const& other)
    requires(CanBePlacedInOptional<U const>)
        : m_pointer(other.ptr())
    {
    }

    template<typename U>
    ALWAYS_INLINE constexpr Optional(Optional<U>&& other)
    requires(CanBePlacedInOptional<U>)
        : m_pointer(other.ptr())
    {
        other.m_pointer = nullptr;
    }

    template<typename U>
    ALWAYS_INLINE constexpr Optional& operator=(Optional<U>& other)
    requires(CanBePlacedInOptional<U>)
    {
        m_pointer = other.ptr();
        return *this;
    }

    template<typename U>
    ALWAYS_INLINE constexpr Optional& operator=(Optional<U> const& other)
    requires(CanBePlacedInOptional<U const>)
    {
        m_pointer = other.ptr();
        return *this;
    }

    template<typename U>
    ALWAYS_INLINE constexpr Optional& operator=(Optional<U>&& other)
    requires(CanBePlacedInOptional<U> && IsLvalueReference<U>)
    {
        m_pointer = other.m_pointer;
        other.m_pointer = nullptr;
        return *this;
    }

    template<typename U>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    ALWAYS_INLINE constexpr Optional& operator=(U& value)
    requires(CanBePlacedInOptional<U>)
    {
        m_pointer = &value;
        return *this;
    }

    // Note: Disallows assignment from a temporary as this does not do any lifetime extension.
    template<typename U>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    ALWAYS_INLINE consteval Optional& operator=(RemoveReference<U> const&& value)
    requires(CanBePlacedInOptional<U>)
    = delete;

    ALWAYS_INLINE constexpr void clear()
    {
        m_pointer = nullptr;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr bool has_value() const { return m_pointer != nullptr; }

    [[nodiscard]] ALWAYS_INLINE RemoveReference<T>* ptr()
    {
        return m_pointer;
    }

    [[nodiscard]] ALWAYS_INLINE RemoveReference<T> const* ptr() const
    {
        return m_pointer;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T value()
    {
        VERIFY(m_pointer);
        return *m_pointer;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr AddConstToReferencedType<T> value() const
    {
        VERIFY(m_pointer);
        return *m_pointer;
    }

    template<typename U>
    requires(IsBaseOf<RemoveCVReference<T>, U>) [[nodiscard]] ALWAYS_INLINE constexpr AddConstToReferencedType<T> value_or(U& fallback) const
    {
        if (m_pointer)
            return value();
        return fallback;
    }

    // Note that this ends up copying the value.
    [[nodiscard]] ALWAYS_INLINE constexpr RemoveCVReference<T> value_or(RemoveCVReference<T> fallback) const
    {
        if (m_pointer)
            return value();
        return fallback;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T release_value()
    {
        return *exchange(m_pointer, nullptr);
    }

    ALWAYS_INLINE constexpr AddConstToReferencedType<T> operator*() const { return value(); }
    ALWAYS_INLINE constexpr T operator*() { return value(); }

    ALWAYS_INLINE RawPtr<AddConst<RemoveReference<T>>> operator->() const { return &value(); }
    ALWAYS_INLINE RawPtr<RemoveReference<T>> operator->() { return &value(); }

    // Conversion operators from Optional<T&> -> Optional<T>, implicit when T is trivially copyable.
    ALWAYS_INLINE constexpr operator Optional<RemoveCVReference<T>>() const
    requires(IsTriviallyCopyable<RemoveCVReference<T>>)
    {
        if (has_value())
            return Optional<RemoveCVReference<T>>(value());
        return {};
    }

    // Conversion operators from Optional<T&> -> Optional<T>, explicit when T is not trivially copyable, since this is usually a mistake.
    ALWAYS_INLINE explicit constexpr operator Optional<RemoveCVReference<T>>() const
    requires(!IsTriviallyCopyable<RemoveCVReference<T>>)
    {
        if (has_value())
            return Optional<RemoveCVReference<T>>(value());
        return {};
    }

    ALWAYS_INLINE constexpr Optional<RemoveCVReference<T>> copy() const
    {
        return static_cast<Optional<RemoveCVReference<T>>>(*this);
    }

    template<typename Callback>
    [[nodiscard]] ALWAYS_INLINE constexpr T value_or_lazy_evaluated(Callback callback) const
    {
        if (m_pointer != nullptr)
            return value();
        return callback();
    }

    template<typename Callback>
    [[nodiscard]] ALWAYS_INLINE constexpr Optional<T> value_or_lazy_evaluated_optional(Callback callback) const
    {
        if (m_pointer != nullptr)
            return value();
        return callback();
    }

    template<typename Callback>
    [[nodiscard]] ALWAYS_INLINE constexpr ErrorOr<T> try_value_or_lazy_evaluated(Callback callback) const
    {
        if (m_pointer != nullptr)
            return value();
        return TRY(callback());
    }

    template<typename Callback>
    [[nodiscard]] ALWAYS_INLINE constexpr ErrorOr<Optional<T>> try_value_or_lazy_evaluated_optional(Callback callback) const
    {
        if (m_pointer != nullptr)
            return value();
        return TRY(callback());
    }

    template<typename F, typename MappedType = decltype(declval<F>()(declval<T&>())), auto IsErrorOr = IsSpecializationOf<MappedType, ErrorOr>, typename OptionalType = Optional<ConditionallyResultType<IsErrorOr, MappedType>>>
    ALWAYS_INLINE constexpr Conditional<IsErrorOr, ErrorOr<OptionalType>, OptionalType> map(F&& mapper)
    {
        if constexpr (IsErrorOr) {
            if (m_pointer != nullptr)
                return OptionalType { TRY(mapper(value())) };
            return OptionalType {};
        } else {
            if (m_pointer != nullptr)
                return OptionalType { mapper(value()) };

            return OptionalType {};
        }
    }

    template<typename F, typename MappedType = decltype(declval<F>()(declval<T&>())), auto IsErrorOr = IsSpecializationOf<MappedType, ErrorOr>, typename OptionalType = Optional<ConditionallyResultType<IsErrorOr, MappedType>>>
    ALWAYS_INLINE constexpr Conditional<IsErrorOr, ErrorOr<OptionalType>, OptionalType> map(F&& mapper) const
    {
        if constexpr (IsErrorOr) {
            if (m_pointer != nullptr)
                return OptionalType { TRY(mapper(value())) };
            return OptionalType {};
        } else {
            if (m_pointer != nullptr)
                return OptionalType { mapper(value()) };

            return OptionalType {};
        }
    }

private:
    RemoveReference<T>* m_pointer { nullptr };
};

template<typename T1, typename T2>
ALWAYS_INLINE constexpr bool operator==(Optional<T1> const& first, Optional<T2> const& second)
{
    return first.has_value() == second.has_value()
        && (!first.has_value() || first.value() == second.value());
}

template<typename T1, typename T2>
ALWAYS_INLINE constexpr bool operator==(Optional<T1> const& first, T2 const& second)
{
    return first.has_value() && first.value() == second;
}

template<typename T>
ALWAYS_INLINE constexpr bool operator==(Optional<T> const& first, OptionalNone)
{
    return !first.has_value();
}

}

#if USING_AK_GLOBALLY
using AK::Optional;
using AK::OptionalNone;
#endif
