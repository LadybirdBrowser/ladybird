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
#include <string.h>

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

template<bool want_fast_last_access, typename StorageType>
struct VectorMetadata {
    StorageType* outline_buffer { nullptr };
};

template<typename StorageType>
struct VectorMetadata<true, StorageType> {
    StorageType* last_slot { nullptr };
    StorageType* outline_buffer { nullptr };
};

}

template<typename T, size_t inline_capacity, FastLastAccess requested_fast_last_access>
requires(!IsRvalueReference<T>) class Vector {
private:
    static constexpr bool contains_reference = IsLvalueReference<T>;
    using StorageType = Conditional<contains_reference, RawPtr<RemoveReference<T>>, T>;

    using VisibleType = RemoveReference<T>;

    template<typename U>
    static constexpr bool CanBePlacedInsideVector = Detail::CanBePlacedInsideVectorHelper<StorageType, contains_reference>::template value<U>;
    static constexpr auto want_fast_last_access = requested_fast_last_access == FastLastAccess::Yes;

public:
    using ValueType = T;
    Vector()
    {
        if constexpr (inline_capacity > 0)
            update_metadata();
    }

    Vector(std::initializer_list<T> list)
    requires(!IsLvalueReference<T>)
    {
        if constexpr (inline_capacity > 0)
            update_metadata();
        ensure_capacity(list.size());
        for (auto& item : list)
            unchecked_append(item);
    }

    Vector(Vector&& other)
        : m_size(other.m_size)
        , m_capacity(other.m_capacity)
        , m_metadata(other.m_metadata)
    {
        if constexpr (inline_capacity > 0) {
            if (!m_metadata.outline_buffer) {
                TypedTransfer<T>::move(inline_buffer(), other.inline_buffer(), m_size);
                TypedTransfer<T>::delete_(other.inline_buffer(), m_size);
            }
            update_metadata();
        }
        other.m_metadata = {};
        other.m_size = 0;
        other.reset_capacity();
    }

    Vector(Vector const& other)
    {
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
        if (m_capacity > 0)
            update_metadata();
    }

    explicit Vector(ReadonlySpan<T> other)
    requires(!IsLvalueReference<T>)
    {
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
        if (m_capacity > 0)
            update_metadata();
    }

    template<size_t other_inline_capacity, FastLastAccess other_requested_fast_last_access>
    Vector(Vector<T, other_inline_capacity, other_requested_fast_last_access> const& other)
    {
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
        if (m_capacity > 0)
            update_metadata();
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
            return m_metadata.outline_buffer ? m_metadata.outline_buffer : inline_buffer();
        return m_metadata.outline_buffer;
    }

    ALWAYS_INLINE StorageType const* data() const
    {
        if constexpr (inline_capacity > 0)
            return m_metadata.outline_buffer ? m_metadata.outline_buffer : inline_buffer();
        return m_metadata.outline_buffer;
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

    VisibleType const& last() const
    {
        if constexpr (want_fast_last_access) {
            VERIFY(m_metadata.last_slot);
            if constexpr (contains_reference)
                return **m_metadata.last_slot;
            else
                return *m_metadata.last_slot;
        } else {
            return at(m_size - 1);
        }
    }
    VisibleType& last()
    {
        if constexpr (want_fast_last_access) {
            VERIFY(m_metadata.last_slot);
            if constexpr (contains_reference)
                return **m_metadata.last_slot;
            else
                return *m_metadata.last_slot;
        } else {
            return at(m_size - 1);
        }
    }

    VisibleType const& unsafe_last() const
    requires(want_fast_last_access)
    {
        return *m_metadata.last_slot;
    }
    VisibleType& unsafe_last()
    requires(want_fast_last_access)
    {
        return *m_metadata.last_slot;
    }

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

    template<typename TUnaryPredicate>
    bool contains(TUnaryPredicate&& predicate) const
    requires(IsCallableWithArguments<TUnaryPredicate, bool, VisibleType const&>)
    {
        return !find_if(forward<TUnaryPredicate>(predicate)).is_end();
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
        ASSERT(m_size < capacity());
        if constexpr (want_fast_last_access) {
            ++m_metadata.last_slot;
            ++m_size;
        } else {
            ++m_size;
        }

        StorageType* last_slot;
        if constexpr (want_fast_last_access)
            last_slot = m_metadata.last_slot;
        else
            last_slot = slot(m_size - 1);

        if constexpr (contains_reference) {
            new (last_slot) StorageType(&value);
        } else {
            new (last_slot) StorageType(forward<U>(value));
        }
    }

    ALWAYS_INLINE void unchecked_append(StorageType const* values, size_t count)
    {
        if (count == 0)
            return;
        VERIFY((size() + count) <= capacity());
        TypedTransfer<StorageType>::copy(slot(m_size), values, count);
        m_size += count;
        update_metadata(); // We have *some* space, since we're appending.
    }

    template<class... Args>
    void empend(Args&&... args)
    requires(!contains_reference)
    {
        MUST(try_empend(forward<Args>(args)...));
    }

    template<class... Args>
    ALWAYS_INLINE void unchecked_empend(Args&&... args)
    requires(!contains_reference)
    {
        VERIFY(m_size < capacity());
        if constexpr (want_fast_last_access) {
            ++m_metadata.last_slot;
            ++m_size;
        } else {
            ++m_size;
        }

        StorageType* last_slot;
        if constexpr (want_fast_last_access)
            last_slot = m_metadata.last_slot;
        else
            last_slot = slot(m_size - 1);

        new (last_slot) StorageType(forward<Args>(args)...);
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
            m_metadata = other.m_metadata;
            if constexpr (inline_capacity > 0) {
                if (!m_metadata.outline_buffer) {
                    for (size_t i = 0; i < m_size; ++i) {
                        new (&inline_buffer()[i]) StorageType(move(other.inline_buffer()[i]));
                        other.inline_buffer()[i].~StorageType();
                    }
                    update_metadata();
                }
            }
            other.m_metadata = {};
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
            if (m_capacity > 0)
                update_metadata();
        }
        return *this;
    }

    template<size_t other_inline_capacity, FastLastAccess other_requested_fast_last_access>
    Vector& operator=(Vector<T, other_inline_capacity, other_requested_fast_last_access> const& other)
    {
        clear();
        ensure_capacity(other.size());
        TypedTransfer<StorageType>::copy(data(), other.data(), other.size());
        m_size = other.size();
        if (m_capacity > 0)
            update_metadata();
        return *this;
    }

    void clear()
    {
        clear_with_capacity();
        if (m_metadata.outline_buffer) {
            kfree_sized(m_metadata.outline_buffer, m_capacity * sizeof(StorageType));
            m_metadata.outline_buffer = nullptr;
        }
        reset_capacity();
    }

    void clear_with_capacity()
    {
        if constexpr (!IsTriviallyDestructible<StorageType>) {
            for (size_t i = 0; i < m_size; ++i)
                data()[i].~StorageType();
        }
        m_size = 0;
        if (m_capacity != 0)
            update_metadata();
    }

    void remove(size_t index)
    {
        VERIFY(index < m_size);

        if constexpr (IsTriviallyDestructible<StorageType> && IsTriviallyCopyable<StorageType>) {
            TypedTransfer<StorageType>::copy(slot(index), slot(index + 1), m_size - index - 1);
        } else {
            at(index).~StorageType();
            for (size_t i = index + 1; i < m_size; ++i) {
                new (slot(i - 1)) StorageType(move(at(i)));
                at(i).~StorageType();
            }
        }

        --m_size;
        update_metadata(); // We have *some* space, we just removed something that was there.
    }

    void remove(size_t index, size_t count)
    {
        if (count == 0)
            return;
        VERIFY(index + count > index);
        VERIFY(index + count <= m_size);

        if constexpr (IsTriviallyDestructible<StorageType> && IsTriviallyCopyable<StorageType>) {
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
        update_metadata(); // We have *some* space, we just removed something that was there.
    }

    /// The iterator pair identify a set of indices *in ascending order* to be removed.
    template<typename It, IteratorPairWith<It> EndIt, typename ToIndex = decltype (*declval<It>()) (*)(It const&)>
    void remove_all(It&& it, EndIt const& end, ToIndex const& to_index = [](It const& it) -> decltype(auto) { return *it; })
    {
        if (!(it != end))
            return;
        size_t write_index = to_index(it);
        VERIFY(write_index < m_size);
        raw_at(write_index).~StorageType();
        ++it;
        size_t next_remove_index = it != end ? to_index(it) : m_size;
        for (auto read_index = write_index + 1; read_index < m_size; ++read_index) {
            if (read_index == next_remove_index) {
                raw_at(read_index).~StorageType();
                ++it;
                next_remove_index = it != end ? to_index(it) : m_size;
            } else {
                if constexpr (IsTriviallyDestructible<StorageType> && IsTriviallyCopyable<StorageType>) {
                    __builtin_memcpy(slot(write_index), slot(read_index), sizeof(StorageType));
                } else {
                    new (slot(write_index)) StorageType(move(raw_at(read_index)));
                    raw_at(read_index).~StorageType();
                }
                ++write_index;
            }
        }

        VERIFY(!(it != end));
        m_size = write_index;
        update_metadata(); // We have *some* space, we just removed something that was there.
    }

    template<IterableContainer Is, typename ToIndex = decltype (*declval<Is>().begin()) (*)(decltype(declval<Is>().begin()) const&)>
    void remove_all(Is&& is, ToIndex const& to_index = [](auto const& it) -> decltype(auto) { return *it; })
    {
        remove_all(is.begin(), is.end(), to_index);
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
        size_t write_index = 0;
        for (size_t read_index = 0; read_index < m_size; ++read_index) {
            if (predicate(at(read_index))) {
                TypedTransfer<StorageType>::delete_(slot(read_index), 1);
                continue;
            }
            if (read_index != write_index) {
                TypedTransfer<StorageType>::move(slot(write_index), slot(read_index), 1);
                TypedTransfer<StorageType>::delete_(slot(read_index), 1);
            }
            ++write_index;
        }

        if (write_index == m_size)
            return false;

        m_size = write_index;
        update_metadata();
        return true;
    }

    ALWAYS_INLINE T take_last()
    requires(want_fast_last_access)
    {
        VERIFY(m_metadata.last_slot);
        auto value = move(*m_metadata.last_slot);
        if constexpr (!contains_reference)
            last().~T();
        --m_size;
        update_metadata(); // We have *some* space, we just removed something that was there.

        if constexpr (contains_reference)
            return *value;
        else
            return value;
    }

    ALWAYS_INLINE T take_last()
    requires(!want_fast_last_access)
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

    ALWAYS_INLINE T unsafe_take_last()
    requires(want_fast_last_access)
    {
        auto value = move(*m_metadata.last_slot);
        if constexpr (!contains_reference)
            m_metadata.last_slot->~T();
        --m_size;
        update_metadata(); // We have *some* space, we just removed something that was there.

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
        if constexpr (IsTriviallyDestructible<StorageType> && IsTriviallyCopyable<StorageType>) {
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
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
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
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
        return {};
    }

    ErrorOr<void> try_extend(Vector const& other)
    {
        TRY(try_grow_capacity(size() + other.size()));
        TypedTransfer<StorageType>::copy(data() + m_size, other.data(), other.size());
        m_size += other.m_size;
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
        return {};
    }

    ALWAYS_INLINE ErrorOr<void> try_append(T&& value)
    {
        TRY(try_grow_capacity(size() + 1));
        if constexpr (contains_reference)
            new (slot(m_size)) StorageType(&value);
        else
            new (slot(m_size)) StorageType(move(value));
        ++m_size;
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
        return {};
    }

    ALWAYS_INLINE ErrorOr<void> try_append(T const& value)
    requires(!contains_reference)
    {
        return try_append(T(value));
    }

    ALWAYS_INLINE ErrorOr<void> try_append(StorageType const* values, size_t count)
    {
        if (count == 0)
            return {};
        TRY(try_grow_capacity(size() + count));
        TypedTransfer<StorageType>::copy(slot(m_size), values, count);
        m_size += count;
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
        return {};
    }

    template<class... Args>
    ErrorOr<void> try_empend(Args&&... args)
    requires(!contains_reference)
    {
        TRY(try_grow_capacity(m_size + 1));
        new (slot(m_size)) StorageType { forward<Args>(args)... };
        ++m_size;
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
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
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
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
        update_metadata(); // We have *some* space, try_grow_capacity above ensured nonzero.
        return {};
    }

    ALWAYS_INLINE ErrorOr<void> try_grow_capacity(size_t needed_capacity)
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

        if constexpr (IsTriviallyCopyable<StorageType>) {
            TypedTransfer<StorageType>::copy(new_buffer, data(), m_size);
        } else {
            for (size_t i = 0; i < m_size; ++i) {
                new (&new_buffer[i]) StorageType(move(at(i)));
                at(i).~StorageType();
            }
        }
        if (m_metadata.outline_buffer)
            kfree_sized(m_metadata.outline_buffer, m_capacity * sizeof(StorageType));
        m_metadata.outline_buffer = new_buffer;
        m_capacity = new_capacity;
        update_metadata(); // We have *some* space, we just allocated it.
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

        if constexpr (IsTriviallyConstructible<StorageType>) {
            // For trivial types, we can just zero the new memory.
            size_t old_size = size();
            memset(slot(old_size), 0, (new_size - old_size) * sizeof(StorageType));
        } else {
            for (size_t i = size(); i < new_size; ++i)
                new (slot(i)) StorageType {};
        }
        m_size = new_size;
        update_metadata(); // We have *some* space, try_ensure_capacity above ensured nonzero.
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
        update_metadata(); // We have *some* space, try_ensure_capacity above ensured nonzero.
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

        if constexpr (!IsTriviallyDestructible<StorageType>) {
            for (size_t i = new_size; i < size(); ++i)
                at(i).~StorageType();
        }
        m_size = new_size;
        update_metadata(); // We have *some* space, as new_size can't be zero here.
    }

    void unsafe_shrink(size_t new_size)
    requires(want_fast_last_access)
    {
        if constexpr (!IsTriviallyDestructible<StorageType>) {
            for (size_t i = new_size; i < size(); ++i)
                at(i).~StorageType();
        }
        m_size = new_size;
        update_metadata(); // We have at least an allocation, as we are not freeing anything.
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
    ALWAYS_INLINE void reset_capacity()
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

    /// NOTE: Do *not* call unguarded if m_capacity can be zero.
    ALWAYS_INLINE void update_metadata()
    {
        if constexpr (want_fast_last_access) {
            m_metadata.last_slot = slot(m_size) - 1;
        }
    }

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

    Detail::VectorMetadata<want_fast_last_access, StorageType> m_metadata;
    alignas(storage_alignment()) unsigned char m_inline_buffer_storage[storage_size()];
};

template<class... Args>
Vector(Args... args) -> Vector<CommonType<Args...>>;

}

#if USING_AK_GLOBALLY
using AK::Vector;
#endif
