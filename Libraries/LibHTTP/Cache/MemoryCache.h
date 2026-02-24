/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Forward.h>
#include <LibURL/URL.h>

namespace HTTP {

class MemoryCache : public RefCounted<MemoryCache> {
public:
    struct Entry {
        u64 vary_key { 0 };

        u32 status_code { 0 };
        ByteString reason_phrase;
        NonnullRefPtr<HeaderList> request_headers;
        NonnullRefPtr<HeaderList> response_headers;
        ByteBuffer response_body;

        UnixDateTime request_time;
        UnixDateTime response_time;
    };

    static NonnullRefPtr<MemoryCache> create();

    Optional<Entry const&> open_entry(URL::URL const&, StringView method, HeaderList const& request_headers, CacheMode);

    void create_entry(URL::URL const&, StringView method, HeaderList const& request_headers, UnixDateTime request_time, u32 status_code, ByteString reason_phrase, HeaderList const& response_headers);
    void finalize_entry(URL::URL const&, StringView method, HeaderList const& request_headers, u32 status_code, HeaderList const& response_headers, ByteBuffer response_body);

private:
    HashMap<u64, Vector<Entry>, IdentityHashTraits<u64>> m_pending_entries;
    HashMap<u64, Vector<Entry>, IdentityHashTraits<u64>> m_complete_entries;
};

}
