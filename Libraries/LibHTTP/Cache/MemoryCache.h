/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibHTTP/Forward.h>
#include <LibURL/URL.h>

namespace HTTP {

class MemoryCache : public RefCounted<MemoryCache> {
public:
    struct Entry {
        u32 status_code { 0 };
        ByteString reason_phrase;

        NonnullRefPtr<HeaderList> response_headers;
        ByteBuffer response_body;
    };

    static NonnullRefPtr<MemoryCache> create();

    Optional<Entry const&> open_entry(URL::URL const&, StringView method, HeaderList const& request_headers) const;

    void create_entry(URL::URL const&, StringView method, u32 status_code, ByteString reason_phrase, HeaderList const& response_headers);
    void finalize_entry(URL::URL const&, StringView method, ByteBuffer response_body);

private:
    HashMap<u64, Entry> m_pending_entries;
    HashMap<u64, Entry> m_complete_entries;
};

}
