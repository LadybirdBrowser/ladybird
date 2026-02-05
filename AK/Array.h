/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Iterator.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/TypedTransfer.h>

namespace AK {

namespace Detail {

// This type serves as the storage of 0-sized `AK::Array`s. While zero-length `T[0]`
// is accepted as a GNU extension, it causes problems with UBSan in Clang 16.
template<typename T>
struct EmptyArrayStorage {
    T& operator[](size_t) const { VERIFY_NOT_REACHED(); }
    constexpr operator T*() const { return nullptr; }
};

}

template<typename T, size_t Size>
struct Array {
    using ValueType = T;

    // This is a static function because constructors mess up Array's POD-ness.
    static Array from_span(ReadonlySpan<T> span)
    {
        Array array;
        VERIFY(span.size() == Size);
        TypedTransfer<T>::copy(array.data(), span.data(), Size);
        return array;
    }

    static constexpr Array from_repeated_value(T const& value)
    {
        Array array;
        array.fill(value);
        return array;
    }

    [[nodiscard]] constexpr T const* data() const { return __data; }
    [[nodiscard]] constexpr T* data() { return __data; }

    [[nodiscard]] constexpr size_t size() const { return Size; }

    [[nodiscard]] constexpr ReadonlySpan<T> span() const { return { __data, Size }; }
    [[nodiscard]] constexpr Span<T> span() { return { __data, Size }; }

    [[nodiscard]] constexpr T const& at(size_t index) const
    {
        VERIFY(index < size());
        return __data[index];
    }
    [[nodiscard]] constexpr T& at(size_t index)
    {
        VERIFY(index < size());
        return __data[index];
    }

    [[nodiscard]] constexpr T const& first() const { return at(0); }
    [[nodiscard]] constexpr T& first() { return at(0); }

    [[nodiscard]] constexpr T const& last() const
    requires(Size > 0)
    {
        return at(Size - 1);
    }
    [[nodiscard]] constexpr T& last()
    requires(Size > 0)
    {
        return at(Size - 1);
    }

    [[nodiscard]] constexpr bool is_empty() const { return size() == 0; }

    [[nodiscard]] constexpr T const& operator[](size_t index) const { return at(index); }
    [[nodiscard]] constexpr T& operator[](size_t index) { return at(index); }

    template<typename T2, size_t Size2>
    [[nodiscard]] constexpr bool operator==(Array<T2, Size2> const& other) const { return span() == other.span(); }

    using ConstIterator = SimpleIterator<Array const, T const>;
    using Iterator = SimpleIterator<Array, T>;

    [[nodiscard]] constexpr ConstIterator begin() const { return ConstIterator::begin(*this); }
    [[nodiscard]] constexpr Iterator begin() { return Iterator::begin(*this); }

    [[nodiscard]] constexpr ConstIterator end() const { return ConstIterator::end(*this); }
    [[nodiscard]] constexpr Iterator end() { return Iterator::end(*this); }

    [[nodiscard]] constexpr operator ReadonlySpan<T>() const { return span(); }
    [[nodiscard]] constexpr operator Span<T>() { return span(); }

    constexpr size_t fill(T const& value)
    {
        for (size_t idx = 0; idx < Size; ++idx)
            __data[idx] = value;

        return Size;
    }

    [[nodiscard]] constexpr T max() const
    requires(requires(T x, T y) { x < y; })
    {
        static_assert(Size > 0, "No values to max() over");

        T value = __data[0];
        for (size_t i = 1; i < Size; ++i)
            value = AK::max(__data[i], value);
        return value;
    }

    [[nodiscard]] constexpr T min() const
    requires(requires(T x, T y) { x > y; })
    {
        static_assert(Size > 0, "No values to min() over");

        T value = __data[0];
        for (size_t i = 1; i < Size; ++i)
            value = AK::min(__data[i], value);
        return value;
    }

    bool contains_slow(T const& value) const
    {
        return first_index_of(value).has_value();
    }

    Optional<size_t> first_index_of(T const& value) const
    {
        for (size_t i = 0; i < Size; ++i) {
            if (__data[i] == value)
                return i;
        }
        return {};
    }

    Conditional<Size == 0, Detail::EmptyArrayStorage<T>, T[Size]> __data;
};

template<typename T, typename... Types>
Array(T, Types...) -> Array<T, sizeof...(Types) + 1>;

namespace Detail {

template<typename T, size_t... Is>
constexpr auto integer_sequence_generate_array([[maybe_unused]] T const offset, IntegerSequence<T, Is...>) -> Array<T, sizeof...(Is)>
{
    return { { (offset + Is)... } };
}

}

template<typename T, T N>
constexpr auto iota_array(T const offset = {})
{
    static_assert(N >= T {}, "Negative sizes not allowed in iota_array()");
    return Detail::integer_sequence_generate_array<T>(offset, MakeIntegerSequence<T, N>());
}

namespace Detail {

template<typename T, size_t N, size_t... Is>
constexpr auto to_array_impl(T (&&a)[N], IndexSequence<Is...>) -> Array<T, sizeof...(Is)>
{
    return { { a[Is]... } };
}

}

template<typename T, size_t N>
constexpr auto to_array(T (&&a)[N])
{
    return Detail::to_array_impl(move(a), MakeIndexSequence<N>());
}

}

#if USING_AK_GLOBALLY
using AK::Array;
using AK::iota_array;
using AK::to_array;
#endif
