/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Weakable.h>
#include <LibHTTP/Forward.h>

namespace HTTP {

class CacheRequest : public Weakable<CacheRequest> {
public:
    virtual ~CacheRequest() = default;

    virtual bool is_revalidation_request() const = 0;

    virtual void notify_request_unblocked(Badge<DiskCache>) = 0;

    // The time when this request last made progress. The disk cache reads this to distinguish a request that's still
    // filling or revalidating an entry — however slowly — from one that's stalled and will never release the entry.
    virtual Optional<MonotonicTime> last_activity_time() const { return {}; }

protected:
    enum class CacheStatus : u8 {
        Unknown,
        NotCached,
        WrittenToCache,
        ReadFromCache,
    };

    Optional<CacheEntryReader&> m_cache_entry_reader;
    Optional<CacheEntryWriter&> m_cache_entry_writer;
    CacheStatus m_cache_status { CacheStatus::Unknown };
};

}
