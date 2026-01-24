/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS::Bytecode {

// Megamorphic inline cache for property access operations.
// This cache is used when property access exceeds the polymorphic inline cache limit (4 shapes).
// It uses a global hash table with weak references to shapes for fast O(1) lookups.
class MegamorphicCache {
public:
    MegamorphicCache();

    // Cache entry representing a single cached property access
    struct Entry {
        enum class Type : u8 {
            Empty,
            GetOwnProperty,
            GetPropertyInPrototypeChain,
            ChangeOwnProperty,
        };

        GC::Weak<Shape> shape;
        u32 property_offset { 0 };
        u32 shape_dictionary_generation { 0 };
        
        // For prototype chain accesses
        GC::Weak<Object> prototype;
        GC::Weak<PrototypeChainValidity> prototype_chain_validity;
        
        Type type { Type::Empty };
    };

    // Lookup a property in the megamorphic cache
    // Returns the cached entry if found and still valid, otherwise None
    Optional<Entry const&> lookup_get(PropertyKey const& key, Shape const& shape) const;
    Optional<Entry const&> lookup_put(PropertyKey const& key, Shape const& shape) const;

    // Insert or update a property access in the cache
    void insert_get(PropertyKey const& key, Shape const& shape, Entry entry);
    void insert_put(PropertyKey const& key, Shape const& shape, Entry entry);

    // Clear all cache entries (for debugging/testing)
    void clear();

private:
    // Cache configuration
    static constexpr size_t CACHE_SIZE = 4096;  // Must be power of 2
    static constexpr size_t MAX_PROBE_LENGTH = 16;
    static constexpr size_t ENTRIES_PER_LINE = 4;
    static constexpr size_t NUM_CACHE_LINES = CACHE_SIZE / ENTRIES_PER_LINE;

    // Cache line: aligned for CPU cache efficiency
    struct alignas(64) CacheLine {
        AK::Array<Entry, ENTRIES_PER_LINE> entries;
    };

    // Separate caches for get and put to avoid conflicts
    AK::Array<CacheLine, NUM_CACHE_LINES> m_get_cache;
    AK::Array<CacheLine, NUM_CACHE_LINES> m_put_cache;

    // Hash function for PropertyKey + Shape
    [[nodiscard]] size_t hash_for_get(PropertyKey const& key, Shape const& shape) const;
    [[nodiscard]] size_t hash_for_put(PropertyKey const& key, Shape const& shape) const;

    // Internal lookup helper
    Optional<Entry const&> lookup_internal(AK::Array<CacheLine, NUM_CACHE_LINES> const& cache, PropertyKey const& key, Shape const& shape, size_t hash) const;
    void insert_internal(AK::Array<CacheLine, NUM_CACHE_LINES>& cache, PropertyKey const& key, Shape const& shape, size_t hash, Entry entry);
};

}
