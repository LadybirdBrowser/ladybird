/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#cors-preflight-cache
class CORSPreflightCache {
public:
    // https://fetch.spec.whatwg.org/#cache-entry
    struct Entry {
        NetworkPartitionKey network_partition_key;
        URL::Origin origin;
        URL::URL url;
        u64 max_age { 5 };
        bool credentials { false };
        Optional<ByteString> method;
        Optional<ByteString> header_name;
        MonotonicTime created_at;
    };

    static CORSPreflightCache& the();

    // https://fetch.spec.whatwg.org/#concept-cache-match
    [[nodiscard]] bool has_method_cache_entry_match(StringView method, Request const&) const;
    [[nodiscard]] bool has_header_name_cache_entry_match(StringView header_name, Request const&) const;

    // https://fetch.spec.whatwg.org/#concept-cache-create
    void create_entry(Request const&, u64 max_age, Optional<ByteString> method, Optional<ByteString> header_name);

    // https://fetch.spec.whatwg.org/#concept-cache-clear
    void clear_entries(Request const&);

    void clear_all();

private:
    bool entry_matches_request(Entry const&, Request const&) const;

    Vector<Entry> m_entries;
};

}
