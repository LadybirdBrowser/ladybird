/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/Forward.h>
#include <AK/Iterator.h>
#include <AK/Optional.h>
#include <AK/ReverseIterator.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Traits.h>
#include <AK/TypedTransfer.h>
#include <AK/kmalloc.h>
#include <initializer_list>

namespace AK {

namespace Detail {

template<typename StorageType, bool>
struct CanBePlacedInsideVectorHelper;

template<typename StorageType>
struct CanBePlacedInsideVectorHelper<StorageType, true> {
    template<typename U>
    static constexpr bool value = requires(U&& u) { StorageType { &u }; };
};

template<typename StorageType>
struct CanBePlacedInsideVectorHelper<StorageType, false> {
    template<typename U>
    static constexpr bool value = requires(U&& u) { StorageType(forward<U>(u)); };
};

}

template<typename T, size_t inline_capacity>
requires(!IsRvalueReference<T>) class Vector {
private:
    static constexpr bool contains_reference = IsLvalueReference<T>;
    using StorageType = Conditional<contains_reference, RawPtr<RemoveReference<T>>, T>;

    using VisibleType = RemoveReference<T>;

    template<typename U>
    static constexpr bool CanBePlacedInsideVector = Detail::CanBePlacedInsideVectorHelper<StorageType, contains_reference>::template value<U>;

public:
    using ValueType = T;
    Vector()
    {
    }

    Vector(std::initializer_list<T> list)
    requires(!IsLvalueReference<T>)
    {
        ensure_capacity(list.size());
        for (auto& item : list)
            unchecked_append(item);
    }

    Vector(Vector&& other)
        : m_size(other.m_size)
        , m_capacity(other.m_capacity)
        , m_outline_buffer(other.m_outline_buffer)
    {
        if constexpr (inline_capacity > 0) {
            if (!m_outline_buffer) {
                TypedTransfer<T>::move(inline_buffer(), other.inline_buffer(), m_size);
                TypedTransfer<T>::delete_(other.inline_buffer(), m_size);
            }
        }
        other.m_outline_buffer = nullptr;
        other.m_size = 0;
        other.reset_capacity();
    }

    Vector(Vector const& other)
    {
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
    }

    explicit Vector(ReadonlySpan<T> other)
    requires(!IsLvalueReference<T>)
    {
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
    }

    template<size_t other_inline_capacity>
    Vector(Vector<T, other_inline_capacity> const& other)
    {
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
    }

    ~Vector()
    {
        clear();
    }

    Span<StorageType> span() { return { data(), size() }; }
    ReadonlySpan<StorageType> span() const { return { data(), size() }; }

    operator Span<StorageType>() { return span(); }
    operator ReadonlySpan<StorageType>() const { return span(); }

    bool is_empty() const { return size() == 0; }
    ALWAYS_INLINE size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }

    ALWAYS_INLINE StorageType* data()
    {
        if constexpr (inline_capacity > 0)
            return m_outline_buffer ? m_outline_buffer : inline_buffer();
        return m_outline_buffer;
    }

    ALWAYS_INLINE StorageType const* data() const
    {
        if constexpr (inline_capacity > 0)
            return m_outline_buffer ? m_outline_buffer : inline_buffer();
        return m_outline_buffer;
    }

    ALWAYS_INLINE VisibleType const& at(size_t i) const
    {
        VERIFY(i < m_size);
        if constexpr (contains_reference)
            return *data()[i];
        else
            return data()[i];
    }

    ALWAYS_INLINE VisibleType& at(size_t i)
    {
        VERIFY(i < m_size);
        if constexpr (contains_reference)
            return *data()[i];
        else
            return data()[i];
    }

    ALWAYS_INLINE VisibleType const& operator[](size_t i) const { return at(i); }
    ALWAYS_INLINE VisibleType& operator[](size_t i) { return at(i); }

    Optional<VisibleType&> get(size_t i)
    {
        if (i >= size())
            return {};
        return at(i);
    }

    Optional<VisibleType const&> get(size_t i) const
    {
        if (i >= size())
            return {};
        return at(i);
    }

    VisibleType const& first() const { return at(0); }
    VisibleType& first() { return at(0); }

    VisibleType const& last() const { return at(size() - 1); }
    VisibleType& last() { return at(size() - 1); }

    template<typename TUnaryPredicate>
    Optional<VisibleType&> first_matching(TUnaryPredicate const& predicate)
    requires(!contains_reference)
    {
        for (size_t i = 0; i < size(); ++i) {
            if (predicate(at(i))) {
                return at(i);
            }
        }
        return {};
    }

    template<typename TUnaryPredicate>
    Optional<VisibleType const&> first_matching(TUnaryPredicate const& predicate) const
    requires(!contains_reference)
    {
        for (size_t i = 0; i < size(); ++i) {
            if (predicate(at(i))) {
                return Optional<VisibleType const&>(at(i));
            }
        }
        return {};
    }

    template<typename TUnaryPredicate>
    Optional<VisibleType const&> last_matching(TUnaryPredicate const& predicate) const
    requires(!contains_reference)
    {
        for (ssize_t i = size() - 1; i >= 0; --i) {
            if (predicate(at(i))) {
                return Optional<VisibleType const&>(at(i));
            }
        }
        return {};
    }

    template<typename V>
    bool operator==(V const& other) const
    {
        if (m_size != other.size())
            return false;
        return TypedTransfer<StorageType>::compare(data(), other.data(), size());
    }

    template<typename V>
    bool contains_slow(V const& value) const
    {
        for (size_t i = 0; i < size(); ++i) {
            if (Traits<VisibleType>::equals(at(i), value))
                return true;
        }
        return false;
    }

    bool contains_in_range(VisibleType const& value, size_t const start, size_t const end) const
    {
        VERIFY(start <= end);
        VERIFY(end < size());
        for (size_t i = start; i <= end; ++i) {
            if (Traits<VisibleType>::equals(at(i), value))
                return true;
        }
        return false;
    }

    template<typename U = T>
    void insert(size_t index, U&& value)
    requires(CanBePlacedInsideVector<U>)
    {
        MUST(try_insert<U>(index, forward<U>(value)));
    }

    template<typename TUnaryPredicate, typename U = T>
    void insert_before_matching(U&& value, TUnaryPredicate const& predicate, size_t first_index = 0, size_t* inserted_index = nullptr)
    requires(CanBePlacedInsideVector<U>)
    {
        MUST(try_insert_before_matching(forward<U>(value), predicate, first_index, inserted_index));
    }

    void extend(Vector&& other)
    {
        MUST(try_extend(move(other)));
    }

    void extend(Vector const& other)
    {
        MUST(try_extend(other));
    }

    ALWAYS_INLINE void append(T&& value)
    {
        if constexpr (contains_reference)
            MUST(try_append(value));
        else
            MUST(try_append(move(value)));
    }

    ALWAYS_INLINE void append(T const& value)
    requires(!contains_reference)
    {
        MUST(try_append(T(value)));
    }

    void append(StorageType const* values, size_t count)
    {
        MUST(try_append(values, count));
    }

    template<typename U = T>
    ALWAYS_INLINE void unchecked_append(U&& value)
    requires(CanBePlacedInsideVector<U>)
    {
        VERIFY((size() + 1) <= capacity());
        if constexpr (contains_reference)
            new (slot(m_size)) StorageType(&value);
        else
            new (slot(m_size)) StorageType(forward<U>(value));
        ++m_size;
    }

    ALWAYS_INLINE void unchecked_append(StorageType const* values, size_t count)
    {
        if (count == 0)
            return;
        VERIFY((size() + count) <= capacity());
        TypedTransfer<StorageType>::copy(slot(m_size), values, count);
        m_size += count;
    }

    template<class... Args>
    void empend(Args&&... args)
    requires(!contains_reference)
    {
        MUST(try_empend(forward<Args>(args)...));
    }

    template<typename U = T>
    void prepend(U&& value)
    requires(CanBePlacedInsideVector<U>)
    {
        MUST(try_insert(0, forward<U>(value)));
    }

    void prepend(Vector&& other)
    {
        MUST(try_prepend(move(other)));
    }

    void prepend(StorageType const* values, size_t count)
    {
        MUST(try_prepend(values, count));
    }

    // FIXME: What about assigning from a vector with lower inline capacity?
    Vector& operator=(Vector&& other)
    {
        if (this != &other) {
            clear();
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            m_outline_buffer = other.m_outline_buffer;
            if constexpr (inline_capacity > 0) {
                if (!m_outline_buffer) {
                    for (size_t i = 0; i < m_size; ++i) {
                        new (&inline_buffer()[i]) StorageType(move(other.inline_buffer()[i]));
                        other.inline_buffer()[i].~StorageType();
                    }
                }
            }
            other.m_outline_buffer = nullptr;
            other.m_size = 0;
            other.reset_capacity();
        }
        return *this;
    }

    Vector& operator=(Vector const& other)
    {
        if (this != &other) {
            clear();
            ensure_capacity(other.size());
            TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
            m_size = other.size();
        }
        return *this;
    }

    template<size_t other_inline_capacity>
    Vector& operator=(Vector<T, other_inline_capacity> const& other)
    {
        clear();
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
        return *this;
    }

    void clear()
    {
        clear_with_capacity();
        if (m_outline_buffer) {
            kfree_sized(m_outline_buffer, m_capacity * sizeof(StorageType));
            m_outline_buffer = nullptr;
        }
        reset_capacity();
    }

    void clear_with_capacity()
    {
        for (size_t i = 0; i < m_size; ++i)
            data()[i].~StorageType();
        m_size = 0;
    }

    void remove(size_t index)
    {
        VERIFY(index < m_size);

        if constexpr (Traits<StorageType>::is_trivial()) {
            TypedTransfer<StorageType>::copy(slot(index), slot(index + 1), m_size - index - 1);
        } else {
            at(index).~StorageType();
            for (size_t i = index + 1; i < m_size; ++i) {
                new (slot(i - 1)) StorageType(move(at(i)));
                at(i).~StorageType();
            }
        }

        --m_size;
    }

    void remove(size_t index, size_t count)
    {
        if (count == 0)
            return;
        VERIFY(index + count > index);
        VERIFY(index + count <= m_size);

        if constexpr (Traits<StorageType>::is_trivial()) {
            TypedTransfer<StorageType>::copy(slot(index), slot(index + count), m_size - index - count);
        } else {
            for (size_t i = index; i < index + count; i++)
                at(i).~StorageType();
            for (size_t i = index + count; i < m_size; ++i) {
                new (slot(i - count)) StorageType(move(at(i)));
                at(i).~StorageType();
            }
        }

        m_size -= count;
    }

    template<typename TUnaryPredicate>
    bool remove_first_matching(TUnaryPredicate const& predicate)
    {
        for (size_t i = 0; i < size(); ++i) {
            if (predicate(at(i))) {
                remove(i);
                return true;
            }
        }
        return false;
    }

    template<typename TUnaryPredicate>
    bool remove_all_matching(TUnaryPredicate const& predicate)
    {
        bool something_was_removed = false;
        for (size_t i = 0; i < size();) {
            if (predicate(at(i))) {
                remove(i);
                something_was_removed = true;
            } else {
                ++i;
            }
        }
        return something_was_removed;
    }

    ALWAYS_INLINE T take_last()
    {
        VERIFY(!is_empty());
        auto value = move(raw_last());
        if constexpr (!contains_reference)
            last().~T();
        --m_size;
        if constexpr (contains_reference)
            return *value;
        else
            return value;
    }

    T take_first()
    {
        VERIFY(!is_empty());
        auto value = move(raw_first());
        remove(0);
        if constexpr (contains_reference)
            return *value;
        else
            return value;
    }

    T take(size_t index)
    {
        auto value = move(raw_at(index));
        remove(index);
        if constexpr (contains_reference)
            return *value;
        else
            return value;
    }

    T unstable_take(size_t index)
    {
        VERIFY(index < m_size);
        swap(raw_at(index), raw_at(m_size - 1));
        return take_last();
    }

    template<typename U = T>
    ErrorOr<void> try_insert(size_t index, U&& value)
    requires(CanBePlacedInsideVector<U>)
    {
        if (index > size())
            return Error::from_errno(EINVAL);
        if (index == size())
            return try_append(forward<U>(value));
        TRY(try_grow_capacity(size() + 1));
        ++m_size;
        if constexpr (Traits<StorageType>::is_trivial()) {
            TypedTransfer<StorageType>::move(slot(index + 1), slot(index), m_size - index - 1);
        } else {
            for (size_t i = size() - 1; i > index; --i) {
                new (slot(i)) StorageType(move(at(i - 1)));
                at(i - 1).~StorageType();
            }
        }
        if constexpr (contains_reference)
            new (slot(index)) StorageType(&value);
        else
            new (slot(index)) StorageType(forward<U>(value));
        return {};
    }

    template<typename TUnaryPredicate, typename U = T>
    ErrorOr<void> try_insert_before_matching(U&& value, TUnaryPredicate const& predicate, size_t first_index = 0, size_t* inserted_index = nullptr)
    requires(CanBePlacedInsideVector<U>)
    {
        for (size_t i = first_index; i < size(); ++i) {
            if (predicate(at(i))) {
                TRY(try_insert(i, forward<U>(value)));
                if (inserted_index)
                    *inserted_index = i;
                return {};
            }
        }
        TRY(try_append(forward<U>(value)));
        if (inserted_index)
            *inserted_index = size() - 1;
        return {};
    }

    ErrorOr<void> try_extend(Vector&& other)
    {
        if (is_empty() && capacity() <= other.capacity()) {
            *this = move(other);
            return {};
        }
        auto other_size = other.size();
        Vector tmp = move(other);
        TRY(try_grow_capacity(size() + other_size));
        TypedTransfer<StorageType>::move(data() + m_size, tmp.data(), other_size);
        m_size += other_size;
        return {};
    }

    ErrorOr<void> try_extend(Vector const& other)
    {
        TRY(try_grow_capacity(size() + other.size()));
        TypedTransfer<StorageType>::copy(data() + m_size, other.data(), other.size());
        m_size += other.m_size;
        return {};
    }

    ErrorOr<void> try_append(T&& value)
    {
        TRY(try_grow_capacity(size() + 1));
        if constexpr (contains_reference)
            new (slot(m_size)) StorageType(&value);
        else
            new (slot(m_size)) StorageType(move(value));
        ++m_size;
        return {};
    }

    ErrorOr<void> try_append(T const& value)
    requires(!contains_reference)
    {
        return try_append(T(value));
    }

    ErrorOr<void> try_append(StorageType const* values, size_t count)
    {
        if (count == 0)
            return {};
        TRY(try_grow_capacity(size() + count));
        TypedTransfer<StorageType>::copy(slot(m_size), values, count);
        m_size += count;
        return {};
    }

    template<class... Args>
    ErrorOr<void> try_empend(Args&&... args)
    requires(!contains_reference)
    {
        TRY(try_grow_capacity(m_size + 1));
        new (slot(m_size)) StorageType { forward<Args>(args)... };
        ++m_size;
        return {};
    }

    template<typename U = T>
    ErrorOr<void> try_prepend(U&& value)
    requires(CanBePlacedInsideVector<U>)
    {
        return try_insert(0, forward<U>(value));
    }

    ErrorOr<void> try_prepend(Vector&& other)
    {
        if (other.is_empty())
            return {};

        if (is_empty()) {
            *this = move(other);
            return {};
        }

        auto other_size = other.size();
        TRY(try_grow_capacity(size() + other_size));

        for (size_t i = size() + other_size - 1; i >= other.size(); --i) {
            new (slot(i)) StorageType(move(at(i - other_size)));
            at(i - other_size).~StorageType();
        }

        Vector tmp = move(other);
        TypedTransfer<StorageType>::move(slot(0), tmp.data(), tmp.size());
        m_size += other_size;
        return {};
    }

    ErrorOr<void> try_prepend(StorageType const* values, size_t count)
    {
        if (count == 0)
            return {};
        TRY(try_grow_capacity(size() + count));
        TypedTransfer<StorageType>::move(slot(count), slot(0), m_size);
        TypedTransfer<StorageType>::copy(slot(0), values, count);
        m_size += count;
        return {};
    }

    ErrorOr<void> try_grow_capacity(size_t needed_capacity)
    {
        if (m_capacity >= needed_capacity)
            return {};
        return try_ensure_capacity(padded_capacity(needed_capacity));
    }

    ErrorOr<void> try_ensure_capacity(size_t needed_capacity)
    {
        if (m_capacity >= needed_capacity)
            return {};
        size_t new_capacity = kmalloc_good_size(needed_capacity * sizeof(StorageType)) / sizeof(StorageType);
        auto* new_buffer = static_cast<StorageType*>(kmalloc_array(new_capacity, sizeof(StorageType)));
        if (new_buffer == nullptr)
            return Error::from_errno(ENOMEM);

        if constexpr (Traits<StorageType>::is_trivial()) {
            TypedTransfer<StorageType>::copy(new_buffer, data(), m_size);
        } else {
            for (size_t i = 0; i < m_size; ++i) {
                new (&new_buffer[i]) StorageType(move(at(i)));
                at(i).~StorageType();
            }
        }
        if (m_outline_buffer)
            kfree_sized(m_outline_buffer, m_capacity * sizeof(StorageType));
        m_outline_buffer = new_buffer;
        m_capacity = new_capacity;
        return {};
    }

    ErrorOr<void> try_resize(size_t new_size, bool keep_capacity = false)
    requires(!contains_reference)
    {
        if (new_size <= size()) {
            shrink(new_size, keep_capacity);
            return {};
        }

        TRY(try_ensure_capacity(new_size));

        for (size_t i = size(); i < new_size; ++i)
            new (slot(i)) StorageType {};
        m_size = new_size;
        return {};
    }

    ErrorOr<void> try_resize_with_default_value(size_t new_size, T const& default_value, bool keep_capacity)
    requires(!contains_reference)
    {
        if (new_size <= size()) {
            shrink(new_size, keep_capacity);
            return {};
        }

        TRY(try_ensure_capacity(new_size));

        for (size_t i = size(); i < new_size; ++i)
            new (slot(i)) StorageType { default_value };
        m_size = new_size;
        return {};
    }

    ErrorOr<void> try_resize_and_keep_capacity(size_t new_size)
    requires(!contains_reference)
    {
        return try_resize(new_size, true);
    }

    void grow_capacity(size_t needed_capacity)
    {
        MUST(try_grow_capacity(needed_capacity));
    }

    void ensure_capacity(size_t needed_capacity)
    {
        MUST(try_ensure_capacity(needed_capacity));
    }

    void shrink(size_t new_size, bool keep_capacity = false)
    {
        VERIFY(new_size <= size());
        if (new_size == size())
            return;

        if (new_size == 0) {
            if (keep_capacity)
                clear_with_capacity();
            else
                clear();
            return;
        }

        for (size_t i = new_size; i < size(); ++i)
            at(i).~StorageType();
        m_size = new_size;
    }

    void resize(size_t new_size, bool keep_capacity = false)
    requires(!contains_reference)
    {
        MUST(try_resize(new_size, keep_capacity));
    }

    void resize_and_keep_capacity(size_t new_size)
    requires(!contains_reference)
    {
        MUST(try_resize_and_keep_capacity(new_size));
    }

    void resize_with_default_value_and_keep_capacity(size_t new_size, T const& default_value)
    requires(!contains_reference)
    {
        MUST(try_resize_with_default_value(new_size, default_value, true));
    }

    void resize_with_default_value(size_t new_size, T const& default_value, bool keep_capacity = false)
    requires(!contains_reference)
    {
        MUST(try_resize_with_default_value(new_size, default_value, keep_capacity));
    }

    void fill(T const& value)
    {
        for (size_t i = 0; i < size(); ++i)
            at(i) = value;
    }

    void shrink_to_fit()
    {
        if (size() == capacity())
            return;
        Vector new_vector;
        new_vector.ensure_capacity(size());
        for (auto& element : *this) {
            new_vector.unchecked_append(move(element));
        }
        *this = move(new_vector);
    }

    using ConstIterator = SimpleIterator<Vector const, VisibleType const>;
    using Iterator = SimpleIterator<Vector, VisibleType>;
    using ReverseIterator = SimpleReverseIterator<Vector, VisibleType>;
    using ReverseConstIterator = SimpleReverseIterator<Vector const, VisibleType const>;

    ConstIterator begin() const { return ConstIterator::begin(*this); }
    Iterator begin() { return Iterator::begin(*this); }
    ReverseIterator rbegin() { return ReverseIterator::rbegin(*this); }
    ReverseConstIterator rbegin() const { return ReverseConstIterator::rbegin(*this); }

    ConstIterator end() const { return ConstIterator::end(*this); }
    Iterator end() { return Iterator::end(*this); }
    ReverseIterator rend() { return ReverseIterator::rend(*this); }
    ReverseConstIterator rend() const { return ReverseConstIterator::rend(*this); }

    ALWAYS_INLINE constexpr auto in_reverse()
    {
        return ReverseWrapper::in_reverse(*this);
    }

    ALWAYS_INLINE constexpr auto in_reverse() const
    {
        return ReverseWrapper::in_reverse(*this);
    }

    template<typename TUnaryPredicate>
    ConstIterator find_if(TUnaryPredicate&& finder) const
    {
        return AK::find_if(begin(), end(), forward<TUnaryPredicate>(finder));
    }

    template<typename TUnaryPredicate>
    Iterator find_if(TUnaryPredicate&& finder)
    {
        return AK::find_if(begin(), end(), forward<TUnaryPredicate>(finder));
    }

    ConstIterator find(VisibleType const& value) const
    {
        return AK::find(begin(), end(), value);
    }

    Iterator find(VisibleType const& value)
    {
        return AK::find(begin(), end(), value);
    }

    Optional<size_t> find_first_index(VisibleType const& value) const
    {
        if (auto const index = AK::find_index(begin(), end(), value);
            index < size()) {
            return index;
        }
        return {};
    }

    template<typename TUnaryPredicate>
    Optional<size_t> find_first_index_if(TUnaryPredicate&& finder) const
    {
        auto maybe_result = AK::find_if(begin(), end(), finder);
        if (maybe_result == end())
            return {};
        return maybe_result.index();
    }

    void reverse()
    {
        for (size_t i = 0; i < size() / 2; ++i)
            AK::swap(at(i), at(size() - i - 1));
    }

private:
    void reset_capacity()
    {
        m_capacity = inline_capacity;
    }

    static size_t padded_capacity(size_t capacity)
    {
        return 4 + capacity + capacity / 4;
    }

    StorageType* slot(size_t i) { return &data()[i]; }
    StorageType const* slot(size_t i) const { return &data()[i]; }

    StorageType* inline_buffer()
    {
        static_assert(inline_capacity > 0);
        return reinterpret_cast<StorageType*>(m_inline_buffer_storage);
    }
    StorageType const* inline_buffer() const
    {
        static_assert(inline_capacity > 0);
        return reinterpret_cast<StorageType const*>(m_inline_buffer_storage);
    }

    StorageType& raw_last() { return raw_at(size() - 1); }
    StorageType& raw_first() { return raw_at(0); }
    StorageType& raw_at(size_t index) { return *slot(index); }

    size_t m_size { 0 };
    size_t m_capacity { inline_capacity };

    static constexpr size_t storage_size()
    {
        if constexpr (inline_capacity == 0)
            return 0;
        else
            return sizeof(StorageType) * inline_capacity;
    }

    static constexpr size_t storage_alignment()
    {
        if constexpr (inline_capacity == 0)
            return 1;
        else
            return alignof(StorageType);
    }

    alignas(storage_alignment()) unsigned char m_inline_buffer_storage[storage_size()];
    StorageType* m_outline_buffer { nullptr };
};

template<class... Args>
Vector(Args... args) -> Vector<CommonType<Args...>>;

}

#if USING_AK_GLOBALLY
using AK::Vector;
#endif
