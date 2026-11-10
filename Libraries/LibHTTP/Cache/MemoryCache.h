/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Forward.h>
#include <LibURL/URL.h>

namespace HTTP {

// What the complete entries of a group of caches hold together.
struct MemoryCacheUsage : public RefCounted<MemoryCacheUsage> {
    // The memory the entries take up, including the response bodies that are not mapped files.
    size_t bytes_in_memory { 0 };
    // The response bodies that are mapped files, whose pages the kernel can drop and read again.
    size_t mapped_bodies { 0 };
};

class MemoryCache : public RefCounted<MemoryCache> {
public:
    struct Entry {
        u64 vary_key { 0 };

        u32 status_code { 0 };
        ByteString reason_phrase;
        NonnullRefPtr<HeaderList> response_headers;
        Core::ImmutableBytes response_body;

        UnixDateTime request_time;
        UnixDateTime response_time;

        // Set when the entry is complete.
        size_t bytes_in_memory { 0 };
        bool body_is_mapped { false };
        u64 last_use { 0 };
    };

    static NonnullRefPtr<MemoryCache> create(NonnullRefPtr<MemoryCacheUsage> = adopt_ref(*new MemoryCacheUsage));
    ~MemoryCache();

    Optional<Entry const&> open_entry(URL::URL const&, StringView method, HeaderList const& request_headers, CacheMode);

    void create_entry(URL::URL const&, StringView method, HeaderList const& request_headers, UnixDateTime request_time, u32 status_code, ByteString reason_phrase, HeaderList const& response_headers);
    void finalize_entry(URL::URL const&, StringView method, HeaderList const& request_headers, u32 status_code, HeaderList const& response_headers, Core::ImmutableBytes response_body);
    // Drops the oldest pending entry with the request's cache and vary keys, for a response whose body will not be
    // finalized. A response that is not stored can still be meant to replace the stored one.
    enum class StoredResponse : u8 {
        Keep,
        Retire,
    };
    void abandon_entry(URL::URL const&, StringView method, HeaderList const& request_headers, HeaderList const& response_headers, StoredResponse);

    void invalidate(URL::URL const&);

    bool is_empty() const { return m_complete_entries.is_empty() && m_pending_entries.is_empty(); }

    struct EntryUse {
        u64 cache_key { 0 };
        u64 vary_key { 0 };
        bool body_is_mapped { false };
        u64 last_use { 0 };
    };
    Vector<EntryUse> complete_entry_uses() const;
    void evict_complete_entry(u64 cache_key, u64 vary_key);

private:
    explicit MemoryCache(NonnullRefPtr<MemoryCacheUsage>);

    void add_to_usage(Entry const&);
    void remove_from_usage(Entry const&);
    Optional<Entry> take_pending_entry(u64 cache_key, Optional<u64> vary_key);
    void remove_complete_entries(u64 cache_key, Function<bool(Entry const&)> const& predicate);

    HashMap<u64, Vector<Entry>, IdentityHashTraits<u64>> m_pending_entries;
    HashMap<u64, Vector<Entry>, IdentityHashTraits<u64>> m_complete_entries;
    NonnullRefPtr<MemoryCacheUsage> m_usage;
};

}
