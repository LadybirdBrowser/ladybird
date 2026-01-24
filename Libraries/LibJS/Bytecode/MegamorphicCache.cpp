/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/MegamorphicCache.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Shape.h>

namespace JS::Bytecode {

MegamorphicCache::MegamorphicCache()
{
    clear();
}

void MegamorphicCache::clear()
{
    for (auto& line : m_get_cache) {
        for (auto& entry : line.entries) {
            entry = {};
        }
    }
    for (auto& line : m_put_cache) {
        for (auto& entry : line.entries) {
            entry = {};
        }
    }
}

// FNV-1a hash function for fast, good distribution
static constexpr size_t fnv1a_hash(void const* data, size_t size, size_t seed = 0xcbf29ce484222325ULL)
{
    size_t hash = seed;
    auto const* bytes = static_cast<u8 const*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;  // FNV prime
    }
    return hash;
}

size_t MegamorphicCache::hash_for_get(PropertyKey const& key, Shape const& shape) const
{
    // Hash combining PropertyKey hash and Shape pointer
    // Using pointer value is safe since we validate shape via weak pointer
    size_t hash = AK::Traits<PropertyKey>::hash(key);
    hash = fnv1a_hash(&shape, sizeof(void*), hash);
    
    // Add a salt to distinguish get from put caches
    hash ^= 0x9e3779b97f4a7c15ULL;
    
    return hash & (NUM_CACHE_LINES - 1);  // Fast modulo for power of 2
}

size_t MegamorphicCache::hash_for_put(PropertyKey const& key, Shape const& shape) const
{
    // Similar to get hash but with different salt
    size_t hash = AK::Traits<PropertyKey>::hash(key);
    hash = fnv1a_hash(&shape, sizeof(void*), hash);
    
    // Different salt for put cache
    hash ^= 0x6a09e667f3bcc908ULL;
    
    return hash & (NUM_CACHE_LINES - 1);
}

Optional<MegamorphicCache::Entry const&> MegamorphicCache::lookup_get(PropertyKey const& key, Shape const& shape) const
{
    auto hash = hash_for_get(key, shape);
    return lookup_internal(m_get_cache, key, shape, hash);
}

Optional<MegamorphicCache::Entry const&> MegamorphicCache::lookup_put(PropertyKey const& key, Shape const& shape) const
{
    auto hash = hash_for_put(key, shape);
    return lookup_internal(m_put_cache, key, shape, hash);
}

Optional<MegamorphicCache::Entry const&> MegamorphicCache::lookup_internal(
    AK::Array<CacheLine, NUM_CACHE_LINES> const& cache,
    PropertyKey const&,
    Shape const& shape,
    size_t hash) const
{
    // Linear probing with limited probe length
    for (size_t probe = 0; probe < MAX_PROBE_LENGTH; ++probe) {
        size_t index = (hash + probe) & (NUM_CACHE_LINES - 1);
        auto const& line = cache[index];

        // Check all entries in this cache line
        for (auto const& entry : line.entries) {
            if (entry.type == Entry::Type::Empty)
                continue;

            // Validate weak pointer is still alive
            auto cached_shape = entry.shape.ptr();
            if (!cached_shape)
                continue;

            // Check if this entry matches
            if (cached_shape != &shape)
                continue;

            // For dictionary shapes, also check generation
            if (shape.is_dictionary()) {
                if (shape.dictionary_generation() != entry.shape_dictionary_generation)
                    continue;
            }

            // Found valid entry!
            return entry;
        }
    }

    return {};
}

void MegamorphicCache::insert_get(PropertyKey const& key, Shape const& shape, Entry entry)
{
    auto hash = hash_for_get(key, shape);
    insert_internal(m_get_cache, key, shape, hash, move(entry));
}

void MegamorphicCache::insert_put(PropertyKey const& key, Shape const& shape, Entry entry)
{
    auto hash = hash_for_put(key, shape);
    insert_internal(m_put_cache, key, shape, hash, move(entry));
}

void MegamorphicCache::insert_internal(
    AK::Array<CacheLine, NUM_CACHE_LINES>& cache,
    PropertyKey const&,
    Shape const&,
    size_t hash,
    Entry entry)
{
    // Linear probing to find an insertion point
    for (size_t probe = 0; probe < MAX_PROBE_LENGTH; ++probe) {
        size_t index = (hash + probe) & (NUM_CACHE_LINES - 1);
        auto& line = cache[index];

        // Try to find an empty slot or invalid entry in this line
        for (auto& existing : line.entries) {
            // Empty slot or dead weak pointer - use it
            if (existing.type == Entry::Type::Empty || !existing.shape.ptr()) {
                existing = move(entry);
                return;
            }
        }
    }

    // Cache is full at this location, evict the last entry in the probe sequence
    // This is simple LRU-ish behavior
    size_t evict_index = (hash + MAX_PROBE_LENGTH - 1) & (NUM_CACHE_LINES - 1);
    auto& evict_line = cache[evict_index];
    evict_line.entries[ENTRIES_PER_LINE - 1] = move(entry);
}

}
