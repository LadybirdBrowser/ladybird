/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheRequest.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibURL/URL.h>

namespace HTTP {

static constexpr auto INDEX_DATABASE = "INDEX"sv;

ErrorOr<DiskCache> DiskCache::create(Mode mode)
{
    auto cache_name = mode == Mode::Normal ? "Cache"sv : "TestCache"sv;
    auto cache_directory = LexicalPath::join(Core::StandardPaths::cache_directory(), "Ladybird"sv, cache_name);

    auto database = TRY(Database::Database::create(cache_directory.string(), INDEX_DATABASE));
    auto index = TRY(CacheIndex::create(database));

    return DiskCache { mode, move(database), move(cache_directory), move(index) };
}

DiskCache::DiskCache(Mode mode, NonnullRefPtr<Database::Database> database, LexicalPath cache_directory, CacheIndex index)
    : m_mode(mode)
    , m_database(move(database))
    , m_cache_directory(move(cache_directory))
    , m_index(move(index))
{
    // Start with a clean slate in test mode.
    if (m_mode == Mode::Testing)
        remove_entries_accessed_since(UnixDateTime::earliest());
}

DiskCache::DiskCache(DiskCache&&) = default;
DiskCache& DiskCache::operator=(DiskCache&&) = default;

DiskCache::~DiskCache() = default;

Variant<Optional<CacheEntryWriter&>, DiskCache::CacheHasOpenEntry> DiskCache::create_entry(CacheRequest& request, URL::URL const& url, StringView method, HeaderList const& request_headers, UnixDateTime request_start_time)
{
    if (!is_cacheable(method))
        return Optional<CacheEntryWriter&> {};

    if (m_mode == Mode::Testing) {
        if (!request_headers.contains(TEST_CACHE_ENABLED_HEADER))
            return Optional<CacheEntryWriter&> {};
    }

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    if (check_if_cache_has_open_entry(request, cache_key, url, CheckReaderEntries::Yes))
        return CacheHasOpenEntry {};

    auto current_time_offset_for_testing = compute_current_time_offset_for_testing(*this, request_headers);
    request_start_time += current_time_offset_for_testing;

    auto cache_entry = CacheEntryWriter::create(*this, m_index, cache_key, move(serialized_url), request_start_time, current_time_offset_for_testing);
    if (cache_entry.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[31;1mUnable to create cache entry for\033[0m {}: {}", url, cache_entry.error());
        return Optional<CacheEntryWriter&> {};
    }

    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[32;1mCreated disk cache entry for\033[0m {}", url);

    auto* cache_entry_pointer = cache_entry.value().ptr();
    m_open_cache_entries.ensure(cache_key).append(cache_entry.release_value());

    return Optional<CacheEntryWriter&> { *cache_entry_pointer };
}

Variant<Optional<CacheEntryReader&>, DiskCache::CacheHasOpenEntry> DiskCache::open_entry(CacheRequest& request, URL::URL const& url, StringView method, HeaderList const& request_headers)
{
    if (!is_cacheable(method))
        return Optional<CacheEntryReader&> {};

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    if (check_if_cache_has_open_entry(request, cache_key, url, CheckReaderEntries::No))
        return CacheHasOpenEntry {};

    auto index_entry = m_index.find_entry(cache_key);
    if (!index_entry.has_value()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[35;1mNo disk cache entry for\033[0m {}", url);
        return Optional<CacheEntryReader&> {};
    }

    auto cache_entry = CacheEntryReader::create(*this, m_index, cache_key, index_entry->response_headers, index_entry->data_size);
    if (cache_entry.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[31;1mUnable to open cache entry for\033[0m {}: {}", url, cache_entry.error());
        m_index.remove_entry(cache_key);

        return Optional<CacheEntryReader&> {};
    }

    auto current_time_offset_for_testing = compute_current_time_offset_for_testing(*this, request_headers);

    auto const& response_headers = cache_entry.value()->response_headers();
    auto freshness_lifetime = calculate_freshness_lifetime(cache_entry.value()->status_code(), response_headers, current_time_offset_for_testing);
    auto current_age = calculate_age(response_headers, index_entry->request_time, index_entry->response_time, current_time_offset_for_testing);

    switch (cache_lifetime_status(response_headers, freshness_lifetime, current_age)) {
    case CacheLifetimeStatus::Fresh:
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[32;1mOpened disk cache entry for\033[0m {} (lifetime={}s age={}s) ({} bytes)", url, freshness_lifetime.to_seconds(), current_age.to_seconds(), index_entry->data_size);
        break;

    case CacheLifetimeStatus::Expired:
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[33;1mCache entry expired for\033[0m {} (lifetime={}s age={}s)", url, freshness_lifetime.to_seconds(), current_age.to_seconds());
        cache_entry.value()->remove();

        return Optional<CacheEntryReader&> {};

    case CacheLifetimeStatus::MustRevalidate:
        // We will hold an exclusive lock on the cache entry for revalidation requests.
        if (check_if_cache_has_open_entry(request, cache_key, url, CheckReaderEntries::Yes))
            return Optional<CacheEntryReader&> {};

        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36;1mMust revalidate disk cache entry for\033[0m {} (lifetime={}s age={}s)", url, freshness_lifetime.to_seconds(), current_age.to_seconds());
        cache_entry.value()->set_must_revalidate();
        break;
    }

    auto* cache_entry_pointer = cache_entry.value().ptr();
    m_open_cache_entries.ensure(cache_key).append(cache_entry.release_value());

    return Optional<CacheEntryReader&> { *cache_entry_pointer };
}

bool DiskCache::check_if_cache_has_open_entry(CacheRequest& request, u64 cache_key, URL::URL const& url, CheckReaderEntries check_reader_entries)
{
    auto open_entries = m_open_cache_entries.get(cache_key);
    if (!open_entries.has_value())
        return false;

    for (auto const& open_entry : *open_entries) {
        if (is<CacheEntryWriter>(*open_entry)) {
            dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36;1mDeferring disk cache entry for\033[0m {} (waiting for existing writer)", url);
            m_requests_waiting_completion.ensure(cache_key).append(request);
            return true;
        }

        // We allow concurrent readers unless another reader is open for revalidation. That reader will issue the network
        // request, which may then result in the cache entry being updated or deleted.
        if (check_reader_entries == CheckReaderEntries::Yes || as<CacheEntryReader>(*open_entry).must_revalidate()) {
            dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36;1mDeferring disk cache entry for\033[0m {} (waiting for existing reader)", url);
            m_requests_waiting_completion.ensure(cache_key).append(request);
            return true;
        }
    }

    return false;
}

Requests::CacheSizes DiskCache::estimate_cache_size_accessed_since(UnixDateTime since)
{
    return m_index.estimate_cache_size_accessed_since(since);
}

void DiskCache::remove_entries_accessed_since(UnixDateTime since)
{
    m_index.remove_entries_accessed_since(since, [&](auto cache_key) {
        if (auto open_entries = m_open_cache_entries.get(cache_key); open_entries.has_value()) {
            for (auto const& open_entry : *open_entries)
                open_entry->mark_for_deletion({});
        }

        auto cache_path = path_for_cache_key(m_cache_directory, cache_key);
        (void)FileSystem::remove(cache_path.string(), FileSystem::RecursionMode::Disallowed);
    });
}

void DiskCache::cache_entry_closed(Badge<CacheEntry>, CacheEntry const& cache_entry)
{
    auto cache_key = cache_entry.cache_key();

    auto open_entries = m_open_cache_entries.get(cache_key);
    if (!open_entries.has_value())
        return;

    open_entries->remove_first_matching([&](auto const& open_entry) { return open_entry.ptr() == &cache_entry; });
    if (open_entries->size() > 0)
        return;

    m_open_cache_entries.remove(cache_key);

    // FIXME: This creates a bit of a first-past-the-post situation if a resumed request causes other pending requests
    //        to become delayed again. We may want to come up with some method to control the order of resumed requests.
    if (auto pending_requests = m_requests_waiting_completion.take(cache_key); pending_requests.has_value()) {
        // We defer resuming requests to ensure we are outside of any internal curl callbacks. For example, when curl
        // invokes the CURLOPT_WRITEFUNCTION callback, we will flush pending HTTP headers to the disk cache. If that
        // does not succeed, we delete the cache entry, and end up here. We must queue the new request outside of that
        // callback, otherwise curl will return CURLM_RECURSIVE_API_CALL error codes.
        Core::deferred_invoke([pending_requests = pending_requests.release_value()]() {
            for (auto const& request : pending_requests) {
                if (request)
                    request->notify_request_unblocked({});
            }
        });
    }
}

}
