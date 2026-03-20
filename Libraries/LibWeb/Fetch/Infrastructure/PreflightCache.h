/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-cache
class PreflightCache {
public:
    static constexpr AK::Duration MAX_AGE_LIMIT = AK::Duration::from_seconds(7200); // 2 hours.

    static PreflightCache& the();

    void cache_method(Request const&, ByteString const& method, AK::Duration max_age);
    void cache_header_name(Request const&, ByteString const& header_name, AK::Duration max_age);

    bool is_a_method_cache_entry_match(Request const&, ByteString const& method);
    bool is_a_header_name_cache_entry_match(Request const&, ByteString const& header_name);

    void clear_cache_entries(Request const&);

    void evict_expired_entries();

private:
    // https://fetch.spec.whatwg.org/#cache-entry
    struct Entry {
        // "Cache entries must be removed after the seconds specified in their max-age field have passed since
        //  storing the entry. Cache entries may be removed before that moment arrives."
        MonotonicTime stored_at;

        // A cache entry consists of:

        // key (a network partition key)
        NetworkPartitionKey key;

        // byte-serialized origin (a byte sequence)
        ByteString byte_serialized_origin;

        // URL (a URL)
        URL::URL url;

        // max-age (a number of seconds)
        AK::Duration max_age;

        // credentials (a boolean)
        bool credentials { false };

        // method (null, `*`, or a method)
        Optional<ByteString> method;

        // header name (null, `*`, or a header name)
        Optional<ByteString> header_name;

        bool has_expired() const;
        void update_max_age(AK::Duration max_age);
    };

    void create_a_new_cache_entry(Request const&, AK::Duration max_age, Optional<ByteString> method, Optional<ByteString> header_name);
    Entry* method_cache_entry_match(Request const&, ByteString const& method);
    Entry* header_name_cache_entry_match(Request const&, ByteString const& header_name);
    static bool is_cache_entry_match(Entry const&, Request const&);

    // A CORS-preflight cache is a list of cache entries.
    Vector<Entry> m_entries;
};

}
