/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/DirIterator.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/URL.h>
#include <RequestServer/Cache/DiskCache.h>
#include <RequestServer/Cache/Utilities.h>

namespace RequestServer {

static constexpr auto INDEX_DATABASE = "INDEX"sv;

ErrorOr<DiskCache> DiskCache::create()
{
    auto cache_directory = LexicalPath::join(Core::StandardPaths::cache_directory(), "Ladybird"sv, "Cache"sv);

    auto database = TRY(Database::Database::create(cache_directory.string(), INDEX_DATABASE));
    auto index = TRY(CacheIndex::create(database));

    return DiskCache { move(database), move(cache_directory), move(index) };
}

DiskCache::DiskCache(NonnullRefPtr<Database::Database> database, LexicalPath cache_directory, CacheIndex index)
    : m_database(move(database))
    , m_cache_directory(move(cache_directory))
    , m_index(move(index))
{
}

Optional<CacheEntryWriter&> DiskCache::create_entry(URL::URL const& url, StringView method, u32 status_code, Optional<String> reason_phrase, HTTP::HeaderMap const& headers, UnixDateTime request_time)
{
    if (!is_cacheable(method, status_code, headers))
        return {};

    if (auto freshness = calculate_freshness_lifetime(headers); freshness.is_negative() || freshness.is_zero())
        return {};

    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    auto cache_entry = CacheEntryWriter::create(*this, m_index, cache_key, move(serialized_url), status_code, move(reason_phrase), headers, request_time);
    if (cache_entry.is_error()) {
        dbgln("\033[31;1mUnable to create cache entry for\033[0m {}: {}", url, cache_entry.error());
        return {};
    }

    dbgln("\033[32;1mCreated disk cache entry for\033[0m {}", url);

    auto address = reinterpret_cast<FlatPtr>(cache_entry.value().ptr());
    m_open_cache_entries.set(address, cache_entry.release_value());

    return static_cast<CacheEntryWriter&>(**m_open_cache_entries.get(address));
}

Optional<CacheEntryReader&> DiskCache::open_entry(URL::URL const& url, StringView method)
{
    auto serialized_url = serialize_url_for_cache_storage(url);
    auto cache_key = create_cache_key(serialized_url, method);

    auto index_entry = m_index.find_entry(cache_key);
    if (!index_entry.has_value()) {
        dbgln("\033[35;1mNo disk cache entry for\033[0m {}", url);
        return {};
    }

    auto cache_entry = CacheEntryReader::create(*this, m_index, cache_key, index_entry->data_size);
    if (cache_entry.is_error()) {
        dbgln("\033[31;1mUnable to open cache entry for\033[0m {}: {}", url, cache_entry.error());
        m_index.remove_entry(cache_key);
        return {};
    }

    auto freshness_lifetime = calculate_freshness_lifetime(cache_entry.value()->headers());
    auto current_age = calculate_age(cache_entry.value()->headers(), index_entry->request_time, index_entry->response_time);

    if (!is_response_fresh(freshness_lifetime, current_age)) {
        dbgln("\033[33;1mCache entry expired for\033[0m {} (lifetime={}s age={}s)", url, freshness_lifetime.to_seconds(), current_age.to_seconds());
        cache_entry.value()->remove();
        return {};
    }

    dbgln("\033[32;1mOpened disk cache entry for\033[0m {} (lifetime={}s age={}s) ({} bytes)", url, freshness_lifetime.to_seconds(), current_age.to_seconds(), index_entry->data_size);

    auto address = reinterpret_cast<FlatPtr>(cache_entry.value().ptr());
    m_open_cache_entries.set(address, cache_entry.release_value());

    return static_cast<CacheEntryReader&>(**m_open_cache_entries.get(address));
}

void DiskCache::clear_cache()
{
    for (auto& [_, cache_entry] : m_open_cache_entries)
        cache_entry->mark_for_deletion({});

    m_index.remove_all_entries();

    Core::DirIterator it { m_cache_directory.string(), Core::DirIterator::SkipDots };
    size_t cache_entries { 0 };

    while (it.has_next()) {
        auto entry = it.next_full_path();
        if (LexicalPath { entry }.title() == INDEX_DATABASE)
            continue;

        (void)FileSystem::remove(entry, FileSystem::RecursionMode::Disallowed);
        ++cache_entries;
    }

    dbgln("Cleared {} disk cache entries", cache_entries);
}

void DiskCache::cache_entry_closed(Badge<CacheEntry>, CacheEntry const& cache_entry)
{
    auto address = reinterpret_cast<FlatPtr>(&cache_entry);
    m_open_cache_entries.remove(address);
}

}
