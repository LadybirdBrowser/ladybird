/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Optional.h>
#include <AK/Weakable.h>
#include <LibHTTP/Forward.h>

namespace HTTP {

class CacheRequest : public Weakable<CacheRequest> {
public:
    virtual ~CacheRequest() = default;

    virtual void notify_request_unblocked(Badge<DiskCache>) = 0;

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
