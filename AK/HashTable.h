/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/ReverseIterator.h>
#include <AK/StdLibExtras.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {

enum class HashSetResult {
    InsertedNewEntry,
    ReplacedExistingEntry,
    KeptExistingEntry,
};

enum class HashSetExistingEntryBehavior {
    Keep,
    Replace,
};

// BucketState doubles as both an enum and a probe length value.
// - Free: empty bucket
// - Used (implicit, values 1..254): value-1 represents probe length
// - CalculateLength: same as Used but probe length > 253, so we calculate the actual probe length
enum class BucketState : u8 {
    Free = 0,
    CalculateLength = 255,
};

template<typename HashTableType, typename T, typename BucketType>
class HashTableIterator {
    friend HashTableType;

public:
    bool operator==(HashTableIterator const& other) const { return m_bucket == other.m_bucket; }
    bool operator!=(HashTableIterator const& other) const { return m_bucket != other.m_bucket; }
    T& operator*() { return *m_bucket->slot(); }
    T* operator->() { return m_bucket->slot(); }
    void operator++() { skip_to_next(); }

private:
    void skip_to_next()
    {
        if (!m_bucket)
            return;
        do {
            ++m_bucket;
            if (m_bucket == m_end_bucket) {
                m_bucket = nullptr;
                return;
            }
        } while (m_bucket->state == BucketState::Free);
    }

    HashTableIterator(BucketType* bucket, BucketType* end_bucket)
        : m_bucket(bucket)
        , m_end_bucket(end_bucket)
    {
    }

    BucketType* m_bucket { nullptr };
    BucketType* m_end_bucket { nullptr };
};

template<typename OrderedHashTableType, typename T, typename BucketType>
class OrderedHashTableIterator {
    friend OrderedHashTableType;

public:
    bool operator==(OrderedHashTableIterator const& other) const { return m_bucket == other.m_bucket; }
    bool operator!=(OrderedHashTableIterator const& other) const { return m_bucket != other.m_bucket; }
    T& operator*() { return *m_bucket->slot(); }
    T* operator->() { return m_bucket->slot(); }
    void operator++() { m_bucket = m_bucket->next; }
    void operator--() { m_bucket = m_bucket->previous; }

private:
    OrderedHashTableIterator(BucketType* bucket, BucketType*)
        : m_bucket(bucket)
    {
    }

    BucketType* m_bucket { nullptr };
};

template<typename OrderedHashTableType, typename T, typename BucketType>
class ReverseOrderedHashTableIterator {
    friend OrderedHashTableType;

public:
    bool operator==(ReverseOrderedHashTableIterator const& other) const { return m_bucket == other.m_bucket; }
    bool operator!=(ReverseOrderedHashTableIterator const& other) const { return m_bucket != other.m_bucket; }
    T& operator*() { return *m_bucket->slot(); }
    T* operator->() { return m_bucket->slot(); }
    void operator++() { m_bucket = m_bucket->previous; }
    void operator--() { m_bucket = m_bucket->next; }

private:
    ReverseOrderedHashTableIterator(BucketType* bucket)
        : m_bucket(bucket)
    {
    }

    BucketType* m_bucket { nullptr };
};

// A set datastructure based on a hash table with closed hashing.
// HashTable can optionally provide ordered iteration when IsOrdered = true.
// For a (more commonly required) map datastructure with key-value entries, see HashMap.
template<typename T, typename TraitsForT, bool IsOrdered>
class HashTable {
    static constexpr size_t grow_capacity_at_least = 8;
    static constexpr size_t grow_at_load_factor_percent = 80;
    static constexpr size_t grow_capacity_increase_percent = 60;

    struct Bucket {
        BucketState state;
        alignas(T) u8 storage[sizeof(T)];
        T* slot() { return reinterpret_cast<T*>(storage); }
        T const* slot() const { return reinterpret_cast<T const*>(storage); }
    };

    struct OrderedBucket {
        OrderedBucket* previous;
        OrderedBucket* next;
        BucketState state;
        alignas(T) u8 storage[sizeof(T)];
        T* slot() { return reinterpret_cast<T*>(storage); }
        T const* slot() const { return reinterpret_cast<T const*>(storage); }
    };

    using BucketType = Conditional<IsOrdered, OrderedBucket, Bucket>;

    struct CollectionData {
    };

    struct OrderedCollectionData {
        BucketType* head { nullptr };
        BucketType* tail { nullptr };
    };

    using CollectionDataType = Conditional<IsOrdered, OrderedCollectionData, CollectionData>;

public:
    HashTable() = default;
    explicit HashTable(size_t capacity) { rehash(capacity); }

    ~HashTable()
    {
        if (!m_buckets)
            return;

        if constexpr (!IsTriviallyDestructible<T>) {
            for (size_t i = 0; i < m_capacity; ++i) {
                if (m_buckets[i].state != BucketState::Free)
                    m_buckets[i].slot()->~T();
            }
        }

        kfree_sized(m_buckets, size_in_bytes(m_capacity));
    }

    HashTable(HashTable const& other)
    {
        rehash(other.capacity());
        for (auto& it : other)
            set(it);
    }

    HashTable& operator=(HashTable const& other)
    {
        HashTable temporary(other);
        swap(*this, temporary);
        return *this;
    }

    HashTable(HashTable&& other) noexcept
        : m_buckets(other.m_buckets)
        , m_collection_data(other.m_collection_data)
        , m_size(other.m_size)
        , m_capacity(other.m_capacity)
    {
        other.m_size = 0;
        other.m_capacity = 0;
        other.m_buckets = nullptr;
        if constexpr (IsOrdered)
            other.m_collection_data = { nullptr, nullptr };
    }

    HashTable& operator=(HashTable&& other) noexcept
    {
        HashTable temporary { move(other) };
        swap(*this, temporary);
        return *this;
    }

    friend void swap(HashTable& a, HashTable& b) noexcept
    {
        swap(a.m_buckets, b.m_buckets);
        swap(a.m_size, b.m_size);
        swap(a.m_capacity, b.m_capacity);

        if constexpr (IsOrdered)
            swap(a.m_collection_data, b.m_collection_data);
    }

    [[nodiscard]] bool is_empty() const { return m_size == 0; }
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] size_t capacity() const { return m_capacity; }

    template<typename U, size_t N>
    ErrorOr<void> try_set_from(U (&from_array)[N])
    {
        for (size_t i = 0; i < N; ++i)
            TRY(try_set(from_array[i]));
        return {};
    }
    template<typename U, size_t N>
    void set_from(U (&from_array)[N])
    {
        MUST(try_set_from(from_array));
    }

    ErrorOr<void> try_ensure_capacity(size_t capacity)
    {
        // The user usually expects "capacity" to mean the number of values that can be stored in a
        // container without it needing to reallocate. Our definition of "capacity" is the number of
        // buckets we can store, but we reallocate earlier because of `grow_at_load_factor_percent`.
        // This calculates the required internal capacity to store `capacity` number of values.
        size_t required_capacity = capacity * 100 / grow_at_load_factor_percent + 1;
        if (required_capacity <= m_capacity)
            return {};
        return try_rehash(required_capacity);
    }
    void ensure_capacity(size_t capacity)
    {
        MUST(try_ensure_capacity(capacity));
    }

    [[nodiscard]] bool contains(T const& value) const
    {
        return find(value) != end();
    }

    template<Concepts::HashCompatible<T> K>
    requires(IsSame<TraitsForT, Traits<T>>) [[nodiscard]] bool contains(K const& value) const
    {
        return find(value) != end();
    }

    using Iterator = Conditional<IsOrdered,
        OrderedHashTableIterator<HashTable, T, BucketType>,
        HashTableIterator<HashTable, T, BucketType>>;

    [[nodiscard]] Iterator begin()
    {
        if constexpr (IsOrdered)
            return Iterator(m_collection_data.head, end_bucket());

        for (size_t i = 0; i < m_capacity; ++i) {
            if (m_buckets[i].state != BucketState::Free)
                return Iterator(&m_buckets[i], end_bucket());
        }
        return end();
    }

    [[nodiscard]] Iterator end()
    {
        return Iterator(nullptr, nullptr);
    }

    using ConstIterator = Conditional<IsOrdered,
        OrderedHashTableIterator<HashTable const, T const, BucketType const>,
        HashTableIterator<HashTable const, T const, BucketType const>>;

    [[nodiscard]] ConstIterator begin() const
    {
        if constexpr (IsOrdered)
            return ConstIterator(m_collection_data.head, end_bucket());

        for (size_t i = 0; i < m_capacity; ++i) {
            if (m_buckets[i].state != BucketState::Free)
                return ConstIterator(&m_buckets[i], end_bucket());
        }
        return end();
    }

    [[nodiscard]] ConstIterator end() const
    {
        return ConstIterator(nullptr, nullptr);
    }

    using ReverseIterator = Conditional<IsOrdered,
        ReverseOrderedHashTableIterator<HashTable, T, BucketType>,
        void>;

    [[nodiscard]] ReverseIterator rbegin()
    requires(IsOrdered)
    {
        return ReverseIterator(m_collection_data.tail);
    }

    [[nodiscard]] ReverseIterator rend()
    requires(IsOrdered)
    {
        return ReverseIterator(nullptr);
    }

    auto in_reverse() { return ReverseWrapper::in_reverse(*this); }

    using ReverseConstIterator = Conditional<IsOrdered,
        ReverseOrderedHashTableIterator<HashTable const, T const, BucketType const>,
        void>;

    [[nodiscard]] ReverseConstIterator rbegin() const
    requires(IsOrdered)
    {
        return ReverseConstIterator(m_collection_data.tail);
    }

    [[nodiscard]] ReverseConstIterator rend() const
    requires(IsOrdered)
    {
        return ReverseConstIterator(nullptr);
    }

    auto in_reverse() const { return ReverseWrapper::in_reverse(*this); }

    void clear()
    {
        *this = HashTable();
    }

    void clear_with_capacity()
    {
        if (m_capacity == 0)
            return;
        if constexpr (!IsTriviallyDestructible<T>) {
            for (auto* bucket : *this)
                bucket->~T();
        }
        __builtin_memset(m_buckets, 0, size_in_bytes(m_capacity));
        m_size = 0;

        if constexpr (IsOrdered)
            m_collection_data = { nullptr, nullptr };
    }

    template<typename U = T>
    ErrorOr<HashSetResult> try_set(U&& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace)
    {
        if (should_grow())
            TRY(try_rehash(m_capacity * (100 + grow_capacity_increase_percent) / 100));

        return write_value(forward<U>(value), existing_entry_behavior);
    }
    template<typename U = T>
    HashSetResult set(U&& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace)
    {
        return MUST(try_set(forward<U>(value), existing_entry_behavior));
    }

    template<typename TUnaryPredicate>
    [[nodiscard]] Iterator find(unsigned hash, TUnaryPredicate predicate)
    {
        return Iterator(lookup_with_hash(hash, move(predicate)), end_bucket());
    }

    [[nodiscard]] Iterator find(T const& value)
    {
        if (is_empty())
            return end();
        return find(TraitsForT::hash(value), [&](auto& entry) { return TraitsForT::equals(entry, value); });
    }

    template<typename TUnaryPredicate>
    [[nodiscard]] ConstIterator find(unsigned hash, TUnaryPredicate predicate) const
    {
        return ConstIterator(lookup_with_hash(hash, move(predicate)), end_bucket());
    }

    [[nodiscard]] ConstIterator find(T const& value) const
    {
        if (is_empty())
            return end();
        return find(TraitsForT::hash(value), [&](auto& entry) { return TraitsForT::equals(entry, value); });
    }
    // FIXME: Support for predicates, while guaranteeing that the predicate call
    //        does not call a non trivial constructor each time invoked
    template<Concepts::HashCompatible<T> K>
    requires(IsSame<TraitsForT, Traits<T>>) [[nodiscard]] Iterator find(K const& value)
    {
        if (is_empty())
            return end();
        return find(Traits<K>::hash(value), [&](auto& entry) { return Traits<T>::equals(entry, value); });
    }

    template<Concepts::HashCompatible<T> K, typename TUnaryPredicate>
    requires(IsSame<TraitsForT, Traits<T>>) [[nodiscard]] Iterator find(K const& value, TUnaryPredicate predicate)
    {
        if (is_empty())
            return end();
        return find(Traits<K>::hash(value), move(predicate));
    }

    template<Concepts::HashCompatible<T> K>
    requires(IsSame<TraitsForT, Traits<T>>) [[nodiscard]] ConstIterator find(K const& value) const
    {
        if (is_empty())
            return end();
        return find(Traits<K>::hash(value), [&](auto& entry) { return Traits<T>::equals(entry, value); });
    }

    template<Concepts::HashCompatible<T> K, typename TUnaryPredicate>
    requires(IsSame<TraitsForT, Traits<T>>) [[nodiscard]] ConstIterator find(K const& value, TUnaryPredicate predicate) const
    {
        if (is_empty())
            return end();
        return find(Traits<K>::hash(value), move(predicate));
    }

    bool remove(T const& value)
    {
        auto it = find(value);
        if (it != end()) {
            remove(it);
            return true;
        }
        return false;
    }

    template<Concepts::HashCompatible<T> K>
    requires(IsSame<TraitsForT, Traits<T>>) bool remove(K const& value)
    {
        auto it = find(value);
        if (it != end()) {
            remove(it);
            return true;
        }
        return false;
    }

    // This invalidates the iterator
    void remove(Iterator& iterator)
    {
        auto* bucket = iterator.m_bucket;
        VERIFY(bucket);
        delete_bucket(*bucket);
        iterator.m_bucket = nullptr;
    }

    template<typename TUnaryPredicate>
    bool remove_all_matching(TUnaryPredicate const& predicate)
    {
        bool has_removed_anything = false;
        for (size_t i = 0; i < m_capacity; ++i) {
            auto& bucket = m_buckets[i];
            if (bucket.state == BucketState::Free || !predicate(*bucket.slot()))
                continue;

            delete_bucket(bucket);
            has_removed_anything = true;

            // If a bucket was shifted up, reevaluate this bucket index
            if (bucket.state != BucketState::Free)
                --i;
        }
        return has_removed_anything;
    }

    T take_last()
    requires(IsOrdered)
    {
        VERIFY(!is_empty());
        T element = move(*m_collection_data.tail->slot());
        delete_bucket(*m_collection_data.tail);
        return element;
    }

    T take_first()
    requires(IsOrdered)
    {
        VERIFY(!is_empty());
        T element = move(*m_collection_data.head->slot());
        delete_bucket(*m_collection_data.head);
        return element;
    }

    [[nodiscard]] Vector<T> values() const
    {
        Vector<T> list;
        list.ensure_capacity(size());
        for (auto& value : *this)
            list.unchecked_append(value);
        return list;
    }

private:
    bool should_grow() const { return ((m_size + 1) * 100) >= (m_capacity * grow_at_load_factor_percent); }
    static constexpr size_t size_in_bytes(size_t capacity) { return sizeof(BucketType) * capacity; }

    BucketType* end_bucket()
    {
        if constexpr (IsOrdered)
            return m_collection_data.tail;
        else
            return &m_buckets[m_capacity];
    }
    BucketType const* end_bucket() const
    {
        return const_cast<HashTable*>(this)->end_bucket();
    }

    ErrorOr<void> try_rehash(size_t new_capacity)
    {
        new_capacity = max(new_capacity, m_capacity + grow_capacity_at_least);
        new_capacity = kmalloc_good_size(size_in_bytes(new_capacity)) / sizeof(BucketType);
        VERIFY(new_capacity >= size());

        auto* old_buckets = m_buckets;
        auto old_buckets_size = size_in_bytes(m_capacity);
        Iterator old_iter = begin();

        auto* new_buckets = kcalloc(1, size_in_bytes(new_capacity));
        if (!new_buckets)
            return Error::from_errno(ENOMEM);

        m_buckets = static_cast<BucketType*>(new_buckets);
        m_capacity = new_capacity;

        if constexpr (IsOrdered)
            m_collection_data = { nullptr, nullptr };

        if (!old_buckets)
            return {};

        m_size = 0;
        for (auto it = move(old_iter); it != end(); ++it) {
            write_value(move(*it), HashSetExistingEntryBehavior::Keep);
            it->~T();
        }

        kfree_sized(old_buckets, old_buckets_size);
        return {};
    }
    void rehash(size_t new_capacity)
    {
        MUST(try_rehash(new_capacity));
    }

    template<typename TUnaryPredicate>
    [[nodiscard]] BucketType* lookup_with_hash(unsigned hash, TUnaryPredicate predicate) const
    {
        if (is_empty())
            return nullptr;

        hash %= m_capacity;
        for (;;) {
            auto* bucket = &m_buckets[hash];
            if (bucket->state == BucketState::Free)
                return nullptr;
            if (predicate(*bucket->slot()))
                return bucket;
            if (++hash == m_capacity) [[unlikely]]
                hash = 0;
        }
    }

    size_t used_bucket_probe_length(BucketType const& bucket) const
    {
        VERIFY(bucket.state != BucketState::Free);

        if (bucket.state == BucketState::CalculateLength) {
            size_t ideal_bucket_index = TraitsForT::hash(*bucket.slot()) % m_capacity;

            VERIFY(&bucket >= m_buckets);
            size_t actual_bucket_index = &bucket - m_buckets;

            if (actual_bucket_index < ideal_bucket_index)
                return m_capacity + actual_bucket_index - ideal_bucket_index;
            return actual_bucket_index - ideal_bucket_index;
        }

        return static_cast<u8>(bucket.state) - 1;
    }

    ALWAYS_INLINE constexpr BucketState bucket_state_for_probe_length(size_t probe_length)
    {
        if (probe_length > 253)
            return BucketState::CalculateLength;
        return static_cast<BucketState>(probe_length + 1);
    }

    template<typename U = T>
    HashSetResult write_value(U&& value, HashSetExistingEntryBehavior existing_entry_behavior)
    {
        auto update_collection_for_new_bucket = [&](BucketType& bucket) {
            if constexpr (IsOrdered) {
                if (!m_collection_data.head) [[unlikely]] {
                    m_collection_data.head = &bucket;
                } else {
                    bucket.previous = m_collection_data.tail;
                    m_collection_data.tail->next = &bucket;
                }
                m_collection_data.tail = &bucket;
            }
        };
        auto update_collection_for_swapped_buckets = [&](BucketType* left_bucket, BucketType* right_bucket) {
            if constexpr (IsOrdered) {
                if (m_collection_data.head == left_bucket)
                    m_collection_data.head = right_bucket;
                else if (m_collection_data.head == right_bucket)
                    m_collection_data.head = left_bucket;
                if (m_collection_data.tail == left_bucket)
                    m_collection_data.tail = right_bucket;
                else if (m_collection_data.tail == right_bucket)
                    m_collection_data.tail = left_bucket;

                if (left_bucket->previous) {
                    if (left_bucket->previous == left_bucket)
                        left_bucket->previous = right_bucket;
                    left_bucket->previous->next = left_bucket;
                }
                if (left_bucket->next) {
                    if (left_bucket->next == left_bucket)
                        left_bucket->next = right_bucket;
                    left_bucket->next->previous = left_bucket;
                }

                if (right_bucket->previous && right_bucket->previous != left_bucket)
                    right_bucket->previous->next = right_bucket;
                if (right_bucket->next && right_bucket->next != left_bucket)
                    right_bucket->next->previous = right_bucket;
            }
        };

        auto bucket_index = TraitsForT::hash(value) % m_capacity;
        size_t probe_length = 0;
        for (;;) {
            auto* bucket = &m_buckets[bucket_index];

            // We found a free bucket, write to it and stop
            if (bucket->state == BucketState::Free) {
                new (bucket->slot()) T(forward<U>(value));
                bucket->state = bucket_state_for_probe_length(probe_length);
                update_collection_for_new_bucket(*bucket);
                ++m_size;
                return HashSetResult::InsertedNewEntry;
            }

            // The bucket is already used, does it have an identical value?
            if (TraitsForT::equals(*bucket->slot(), static_cast<T const&>(value))) {
                if (existing_entry_behavior == HashSetExistingEntryBehavior::Replace) {
                    (*bucket->slot()) = forward<U>(value);
                    return HashSetResult::ReplacedExistingEntry;
                }
                return HashSetResult::KeptExistingEntry;
            }

            // Robin hood: if our probe length is larger (poor) than this bucket's (rich), steal its position!
            // This ensures that we will always traverse buckets in order of probe length.
            auto target_probe_length = used_bucket_probe_length(*bucket);
            if (probe_length > target_probe_length) {
                // Copy out bucket
                BucketType bucket_to_move = move(*bucket);
                update_collection_for_swapped_buckets(bucket, &bucket_to_move);

                // Write new bucket
                new (bucket->slot()) T(forward<U>(value));
                bucket->state = bucket_state_for_probe_length(probe_length);
                probe_length = target_probe_length;
                if constexpr (IsOrdered)
                    bucket->next = nullptr;
                update_collection_for_new_bucket(*bucket);
                ++m_size;

                // Find a free bucket, swapping with smaller probe length buckets along the way
                for (;;) {
                    if (++bucket_index == m_capacity) [[unlikely]]
                        bucket_index = 0;
                    bucket = &m_buckets[bucket_index];
                    ++probe_length;

                    if (bucket->state == BucketState::Free) {
                        *bucket = move(bucket_to_move);
                        bucket->state = bucket_state_for_probe_length(probe_length);
                        update_collection_for_swapped_buckets(&bucket_to_move, bucket);
                        break;
                    }

                    target_probe_length = used_bucket_probe_length(*bucket);
                    if (probe_length > target_probe_length) {
                        swap(bucket_to_move, *bucket);
                        bucket->state = bucket_state_for_probe_length(probe_length);
                        probe_length = target_probe_length;
                        update_collection_for_swapped_buckets(&bucket_to_move, bucket);
                    }
                }

                return HashSetResult::InsertedNewEntry;
            }

            // Try next bucket
            if (++bucket_index == m_capacity) [[unlikely]]
                bucket_index = 0;
            ++probe_length;
        }
    }

    void delete_bucket(auto& bucket)
    {
        VERIFY(bucket.state != BucketState::Free);

        // Delete the bucket
        bucket.slot()->~T();
        if constexpr (IsOrdered) {
            if (bucket.previous)
                bucket.previous->next = bucket.next;
            else
                m_collection_data.head = bucket.next;
            if (bucket.next)
                bucket.next->previous = bucket.previous;
            else
                m_collection_data.tail = bucket.previous;
            bucket.previous = nullptr;
            bucket.next = nullptr;
        }
        --m_size;

        // If we deleted a bucket, we need to make sure to shift up all buckets after it to ensure
        // that we can still probe for buckets with collisions, and we automatically optimize the
        // probe lengths. To do so, we shift the following buckets up until we reach a free bucket,
        // or a bucket with a probe length of 0 (the ideal index for that bucket).
        auto update_bucket_neighbors = [&](BucketType* bucket) {
            if constexpr (IsOrdered) {
                if (bucket->previous)
                    bucket->previous->next = bucket;
                else
                    m_collection_data.head = bucket;
                if (bucket->next)
                    bucket->next->previous = bucket;
                else
                    m_collection_data.tail = bucket;
            }
        };

        VERIFY(&bucket >= m_buckets);
        size_t shift_to_index = &bucket - m_buckets;
        VERIFY(shift_to_index < m_capacity);
        size_t shift_from_index = shift_to_index;
        for (;;) {
            if (++shift_from_index == m_capacity) [[unlikely]]
                shift_from_index = 0;

            auto* shift_from_bucket = &m_buckets[shift_from_index];
            if (shift_from_bucket->state == BucketState::Free)
                break;

            auto shift_from_probe_length = used_bucket_probe_length(*shift_from_bucket);
            if (shift_from_probe_length == 0)
                break;

            auto* shift_to_bucket = &m_buckets[shift_to_index];
            *shift_to_bucket = move(*shift_from_bucket);
            if constexpr (IsOrdered) {
                shift_from_bucket->previous = nullptr;
                shift_from_bucket->next = nullptr;
            }
            shift_to_bucket->state = bucket_state_for_probe_length(shift_from_probe_length - 1);
            update_bucket_neighbors(shift_to_bucket);

            if (++shift_to_index == m_capacity) [[unlikely]]
                shift_to_index = 0;
        }

        // Mark last bucket as free
        m_buckets[shift_to_index].state = BucketState::Free;
    }

    BucketType* m_buckets { nullptr };

    NO_UNIQUE_ADDRESS CollectionDataType m_collection_data;
    size_t m_size { 0 };
    size_t m_capacity { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::HashSetResult;
using AK::HashTable;
using AK::OrderedHashTable;
#endif
