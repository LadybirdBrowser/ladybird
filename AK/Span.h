/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Iterator.h>
#include <AK/TypedTransfer.h>
#include <AK/Types.h>

namespace AK {

namespace Detail {

template<typename T>
class Span {
public:
    ALWAYS_INLINE constexpr Span() = default;

    ALWAYS_INLINE constexpr Span(T* values, size_t size)
        : m_values(values)
        , m_size(size)
    {
    }

    template<size_t size>
    ALWAYS_INLINE constexpr Span(T (&values)[size])
        : m_values(values)
        , m_size(size)
    {
    }

    template<size_t size>
    ALWAYS_INLINE constexpr Span(Array<T, size>& array)
        : m_values(array.data())
        , m_size(size)
    {
    }

    template<size_t size>
    requires(IsConst<T>)
    ALWAYS_INLINE constexpr Span(Array<T, size> const& array)
        : m_values(array.data())
        , m_size(size)
    {
    }

protected:
    T* m_values { nullptr };
    size_t m_size { 0 };
};

template<>
class Span<u8> {
public:
    ALWAYS_INLINE constexpr Span() = default;

    ALWAYS_INLINE constexpr Span(u8* values, size_t size)
        : m_values(values)
        , m_size(size)
    {
    }

    ALWAYS_INLINE Span(void* values, size_t size)
        : m_values(reinterpret_cast<u8*>(values))
        , m_size(size)
    {
    }

    template<size_t size>
    ALWAYS_INLINE constexpr Span(u8 (&values)[size])
        : m_values(values)
        , m_size(size)
    {
    }

protected:
    u8* m_values { nullptr };
    size_t m_size { 0 };
};

template<>
class Span<u8 const> {
public:
    ALWAYS_INLINE constexpr Span() = default;

    ALWAYS_INLINE constexpr Span(u8 const* values, size_t size)
        : m_values(values)
        , m_size(size)
    {
    }

    ALWAYS_INLINE Span(void const* values, size_t size)
        : m_values(reinterpret_cast<u8 const*>(values))
        , m_size(size)
    {
    }

    ALWAYS_INLINE Span(char const* values, size_t size)
        : m_values(reinterpret_cast<u8 const*>(values))
        , m_size(size)
    {
    }

    template<size_t size>
    ALWAYS_INLINE constexpr Span(u8 const (&values)[size])
        : m_values(values)
        , m_size(size)
    {
    }

protected:
    u8 const* m_values { nullptr };
    size_t m_size { 0 };
};

}

template<typename T>
class Span : public Detail::Span<T> {
public:
    using Detail::Span<T>::Span;

    constexpr Span() = default;

    [[nodiscard]] ALWAYS_INLINE constexpr T const* data() const { return this->m_values; }
    [[nodiscard]] ALWAYS_INLINE constexpr T* data() { return this->m_values; }

    [[nodiscard]] ALWAYS_INLINE constexpr T const* offset_pointer(size_t offset) const { return this->m_values + offset; }
    [[nodiscard]] ALWAYS_INLINE constexpr T* offset_pointer(size_t offset) { return this->m_values + offset; }

    using ConstIterator = SimpleIterator<Span const, T const>;
    using Iterator = SimpleIterator<Span, T>;

    constexpr ConstIterator begin() const { return ConstIterator::begin(*this); }
    constexpr Iterator begin() { return Iterator::begin(*this); }

    constexpr ConstIterator end() const { return ConstIterator::end(*this); }
    constexpr Iterator end() { return Iterator::end(*this); }

    [[nodiscard]] ALWAYS_INLINE constexpr size_t size() const { return this->m_size; }
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_null() const { return this->m_values == nullptr; }
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_empty() const { return this->m_size == 0; }

    [[nodiscard]] ALWAYS_INLINE constexpr Span slice(size_t start, size_t length) const
    {
        VERIFY(start + length <= size());
        return { this->m_values + start, length };
    }
    [[nodiscard]] ALWAYS_INLINE constexpr Span slice(size_t start) const
    {
        VERIFY(start <= size());
        return { this->m_values + start, size() - start };
    }
    [[nodiscard]] ALWAYS_INLINE constexpr Span slice_from_end(size_t count) const
    {
        VERIFY(count <= size());
        return { this->m_values + size() - count, count };
    }

    [[nodiscard]] ALWAYS_INLINE constexpr Span trim(size_t length) const
    {
        return { this->m_values, min(size(), length) };
    }

    [[nodiscard]] Span align_to(size_t alignment) const
    {
        auto* start = reinterpret_cast<T*>(align_up_to((FlatPtr)data(), alignment));
        auto* end = reinterpret_cast<T*>(align_down_to((FlatPtr)(data() + size()), alignment));
        if (end < start)
            return {};
        size_t length = end - start;
        return { start, length };
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T* offset(size_t start) const
    {
        VERIFY(start < this->m_size);
        return this->m_values + start;
    }

    ALWAYS_INLINE constexpr void overwrite(size_t offset, void const* data, size_t data_size)
    {
        // make sure we're not told to write past the end
        VERIFY(offset + data_size <= size() * sizeof(T));
        TypedTransfer<T>::copy(this->data() + offset, static_cast<T const*>(data), data_size / sizeof(T));
    }

    ALWAYS_INLINE constexpr size_t copy_to(Span<RemoveConst<T>> other) const
    {
        VERIFY(other.size() >= size());
        return TypedTransfer<RemoveConst<T>>::copy(other.data(), data(), size());
    }

    ALWAYS_INLINE constexpr size_t copy_trimmed_to(Span<RemoveConst<T>> other) const
    {
        auto const count = min(size(), other.size());
        return TypedTransfer<RemoveConst<T>>::copy(other.data(), data(), count);
    }

    ALWAYS_INLINE constexpr size_t fill(T const& value)
    {
        for (size_t idx = 0; idx < size(); ++idx)
            data()[idx] = value;

        return size();
    }

    template<typename V>
    [[nodiscard]] constexpr bool contains_slow(V const& value) const
    {
        for (size_t i = 0; i < size(); ++i) {
            if (Traits<RemoveReference<T>>::equals(at(i), value))
                return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool starts_with(ReadonlySpan<T> other) const
    {
        if (size() < other.size())
            return false;

        return TypedTransfer<T>::compare(data(), other.data(), other.size());
    }

    [[nodiscard]] constexpr size_t matching_prefix_length(ReadonlySpan<T> other) const
    {
        auto maximum_length = min(size(), other.size());

        for (size_t i = 0; i < maximum_length; i++) {
            if (data()[i] != other.data()[i])
                return i;
        }

        return maximum_length;
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const& at(size_t index) const
    {
        VERIFY(index < this->m_size);
        return this->m_values[index];
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T& at(size_t index)
    {
        VERIFY(index < this->m_size);
        return this->m_values[index];
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const& first() const
    {
        return this->at(0);
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T& first()
    {
        return this->at(0);
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const& last() const
    {
        return this->at(this->size() - 1);
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T& last()
    {
        return this->at(this->size() - 1);
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T const& operator[](size_t index) const
    {
        return at(index);
    }

    void reverse()
    {
        for (size_t i = 0; i < size() / 2; ++i)
            AK::swap(at(i), at(size() - i - 1));
    }

    [[nodiscard]] ALWAYS_INLINE constexpr T& operator[](size_t index)
    {
        return at(index);
    }

    constexpr bool operator==(Span const& other) const
    {
        if (size() != other.size())
            return false;

        return TypedTransfer<T>::compare(data(), other.data(), size());
    }

    constexpr bool operator==(Span<T const> const& other) const
    requires(!IsConst<T>)
    {
        return Span<T const>(*this) == other;
    }

    ALWAYS_INLINE constexpr operator ReadonlySpan<T>() const
    {
        return { data(), size() };
    }

    template<typename TUnaryPredicate>
    Optional<T&> last_matching(TUnaryPredicate const& predicate)
    {
        for (ssize_t i = size() - 1; i >= 0; --i) {
            if (predicate(at(i))) {
                return at(i);
            }
        }
        return {};
    }

    template<typename TUnaryPredicate>
    Optional<T&> first_matching(TUnaryPredicate const& predicate)
    {
        for (size_t i = 0; i < size(); ++i) {
            if (predicate(at(i))) {
                return at(i);
            }
        }
        return {};
    }
};

template<typename T>
struct Traits<Span<T>> : public DefaultTraits<Span<T>> {
    static unsigned hash(Span<T> const& span)
    {
        unsigned hash = 0;
        for (auto const& value : span) {
            auto value_hash = Traits<T>::hash(value);
            hash = pair_int_hash(hash, value_hash);
        }
        return hash;
    }

    constexpr static bool is_trivial() { return true; }
};

template<typename T>
using ReadonlySpan = Span<T const>;

using ReadonlyBytes = ReadonlySpan<u8>;
using Bytes = Span<u8>;

template<typename T>
requires(IsTrivial<T>)
ReadonlyBytes to_readonly_bytes(Span<T> span)
{
    return ReadonlyBytes { static_cast<void const*>(span.data()), span.size() * sizeof(T) };
}

template<typename T>
requires(IsTrivial<T> && !IsConst<T>)
Bytes to_bytes(Span<T> span)
{
    return Bytes { static_cast<void*>(span.data()), span.size() * sizeof(T) };
}

}

#if USING_AK_GLOBALLY
using AK::Bytes;
using AK::ReadonlyBytes;
using AK::ReadonlySpan;
using AK::Span;
using AK::to_bytes;
using AK::to_readonly_bytes;
#endif
