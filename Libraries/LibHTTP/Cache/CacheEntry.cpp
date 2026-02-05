/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashFunctions.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheEntry.h>
#include <LibHTTP/Cache/CacheIndex.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>

namespace HTTP {

ErrorOr<CacheHeader> CacheHeader::read_from_stream(Stream& stream)
{
    CacheHeader header;
    header.magic = TRY(stream.read_value<u32>());
    header.version = TRY(stream.read_value<u32>());
    header.key_hash = TRY(stream.read_value<u32>());
    header.url_size = TRY(stream.read_value<u32>());
    header.url_hash = TRY(stream.read_value<u32>());
    header.status_code = TRY(stream.read_value<u32>());
    header.reason_phrase_size = TRY(stream.read_value<u32>());
    header.reason_phrase_hash = TRY(stream.read_value<u32>());
    return header;
}

ErrorOr<void> CacheHeader::write_to_stream(Stream& stream) const
{
    TRY(stream.write_value(magic));
    TRY(stream.write_value(version));
    TRY(stream.write_value(key_hash));
    TRY(stream.write_value(url_size));
    TRY(stream.write_value(url_hash));
    TRY(stream.write_value(status_code));
    TRY(stream.write_value(reason_phrase_size));
    TRY(stream.write_value(reason_phrase_hash));
    return {};
}

u32 CacheHeader::hash() const
{
    u32 hash = 0;
    hash = pair_int_hash(hash, magic);
    hash = pair_int_hash(hash, version);
    hash = pair_int_hash(hash, key_hash);
    hash = pair_int_hash(hash, url_size);
    hash = pair_int_hash(hash, url_hash);
    hash = pair_int_hash(hash, status_code);
    hash = pair_int_hash(hash, reason_phrase_size);
    hash = pair_int_hash(hash, reason_phrase_hash);
    return hash;
}

ErrorOr<void> CacheFooter::write_to_stream(Stream& stream) const
{
    TRY(stream.write_value(data_size));
    TRY(stream.write_value(header_hash));
    return {};
}

ErrorOr<CacheFooter> CacheFooter::read_from_stream(Stream& stream)
{
    CacheFooter footer;
    footer.data_size = TRY(stream.read_value<u64>());
    footer.header_hash = TRY(stream.read_value<u32>());
    return footer;
}

CacheEntry::CacheEntry(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 vary_key, String url, Optional<LexicalPath> path, CacheHeader cache_header)
    : m_disk_cache(disk_cache)
    , m_index(index)
    , m_cache_key(cache_key)
    , m_vary_key(vary_key)
    , m_url(move(url))
    , m_path(move(path))
    , m_cache_header(cache_header)
{
}

void CacheEntry::remove()
{
    if (!m_path.has_value())
        return;

    (void)FileSystem::remove(m_path->string(), FileSystem::RecursionMode::Disallowed);
    m_index.remove_entry(m_cache_key, m_vary_key);
}

void CacheEntry::close_and_destroy_cache_entry()
{
    m_disk_cache.cache_entry_closed({}, *this);
}

ErrorOr<NonnullOwnPtr<CacheEntryWriter>> CacheEntryWriter::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, UnixDateTime request_time, AK::Duration current_time_offset_for_testing)
{
    CacheHeader cache_header;
    cache_header.key_hash = u64_hash(cache_key);
    cache_header.url_size = url.byte_count();
    cache_header.url_hash = url.hash();

    return adopt_own(*new CacheEntryWriter { disk_cache, index, cache_key, move(url), cache_header, request_time, current_time_offset_for_testing });
}

CacheEntryWriter::CacheEntryWriter(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, CacheHeader cache_header, UnixDateTime request_time, AK::Duration current_time_offset_for_testing)
    : CacheEntry(disk_cache, index, cache_key, 0, move(url), {}, cache_header)
    , m_request_time(request_time)
    , m_current_time_offset_for_testing(current_time_offset_for_testing)
{
}

ErrorOr<void> CacheEntryWriter::write_status_and_reason(u32 status_code, Optional<String> reason_phrase, HeaderList const& request_headers, HeaderList const& response_headers)
{
    if (m_marked_for_deletion) {
        close_and_destroy_cache_entry();
        return Error::from_string_literal("Cache entry has been deleted");
    }

    m_response_time = UnixDateTime::now() + m_current_time_offset_for_testing;
    m_cache_header.status_code = status_code;

    if (reason_phrase.has_value()) {
        m_cache_header.reason_phrase_size = reason_phrase->byte_count();
        m_cache_header.reason_phrase_hash = reason_phrase->hash();
    }

    auto result = [&]() -> ErrorOr<void> {
        if (!is_cacheable(status_code, response_headers))
            return Error::from_string_literal("Response is not cacheable");

        m_vary_key = create_vary_key(request_headers, response_headers);
        m_path = path_for_cache_entry(m_disk_cache.cache_directory(), m_cache_key, m_vary_key);

        auto freshness_lifetime = calculate_freshness_lifetime(status_code, response_headers, m_current_time_offset_for_testing);
        auto current_age = calculate_age(response_headers, m_request_time, m_response_time, m_current_time_offset_for_testing);

        // We can cache already-expired responses if there are other cache directives that allow us to revalidate the
        // response on subsequent requests. For example, `Cache-Control: max-age=0, must-revalidate`.
        if (cache_lifetime_status(request_headers, response_headers, freshness_lifetime, current_age) == CacheLifetimeStatus::Expired)
            return Error::from_string_literal("Response has already expired");

        auto unbuffered_file = TRY(Core::File::open(m_path->string(), Core::File::OpenMode::Write));
        m_file = TRY(Core::OutputBufferedFile::create(move(unbuffered_file)));

        TRY(m_file->write_value(m_cache_header));
        TRY(m_file->write_until_depleted(m_url));
        if (reason_phrase.has_value())
            TRY(m_file->write_until_depleted(*reason_phrase));

        return {};
    }();

    if (result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to write status/reason to cache entry for\033[0m {}: {}", m_url, result.error());

        remove();
        close_and_destroy_cache_entry();

        return result.release_error();
    }

    return {};
}

ErrorOr<void> CacheEntryWriter::write_data(ReadonlyBytes data)
{
    if (m_marked_for_deletion) {
        close_and_destroy_cache_entry();
        return Error::from_string_literal("Cache entry has been deleted");
    }

    if (auto result = m_file->write_until_depleted(data); result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to write data to cache entry for\033[0m {}: {}", m_url, result.error());

        remove();
        close_and_destroy_cache_entry();

        return result.release_error();
    }

    m_cache_footer.data_size += data.size();
    return {};
}

ErrorOr<void> CacheEntryWriter::flush(NonnullRefPtr<HeaderList> request_headers, NonnullRefPtr<HeaderList> response_headers)
{
    ScopeGuard guard { [&]() { close_and_destroy_cache_entry(); } };

    if (m_marked_for_deletion)
        return Error::from_string_literal("Cache entry has been deleted");

    m_cache_footer.header_hash = m_cache_header.hash();

    if (auto result = m_file->write_value(m_cache_footer); result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to flush cache entry for\033[0m {}: {}", m_url, result.error());
        remove();

        return result.release_error();
    }

    if (auto result = m_index.create_entry(m_cache_key, m_vary_key, m_url, move(request_headers), move(response_headers), m_cache_footer.data_size, m_request_time, m_response_time); result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to flush cache entry for\033[0m {} ({} bytes): {}", m_url, m_cache_footer.data_size, result.error());
        remove();

        return result.release_error();
    }

    m_disk_cache.remove_entries_exceeding_cache_limit();

    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[34;1mFinished caching\033[0m {} ({} bytes)", m_url, m_cache_footer.data_size);
    return {};
}

void CacheEntryWriter::remove_incomplete_entry()
{
    remove();
    close_and_destroy_cache_entry();
}

ErrorOr<NonnullOwnPtr<CacheEntryReader>> CacheEntryReader::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 vary_key, NonnullRefPtr<HeaderList> response_headers, u64 data_size)
{
    auto path = path_for_cache_entry(disk_cache.cache_directory(), cache_key, vary_key);

    auto file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Read));
    auto fd = file->fd();

    CacheHeader cache_header;
    size_t cache_header_size { 0 };

    String url;
    Optional<String> reason_phrase;

    auto result = [&]() -> ErrorOr<void> {
        cache_header = TRY(file->read_value<CacheHeader>());
        cache_header_size = TRY(file->tell());

        if (cache_header.magic != CacheHeader::CACHE_MAGIC)
            return Error::from_string_literal("Magic value mismatch");
        if (cache_header.version != CACHE_VERSION)
            return Error::from_string_literal("Version mismatch");

        if (cache_header.key_hash != u64_hash(cache_key))
            return Error::from_string_literal("Key hash mismatch");

        url = TRY(String::from_stream(*file, cache_header.url_size));
        if (url.hash() != cache_header.url_hash)
            return Error::from_string_literal("URL hash mismatch");

        if (cache_header.reason_phrase_size != 0) {
            reason_phrase = TRY(String::from_stream(*file, cache_header.reason_phrase_size));
            if (reason_phrase->hash() != cache_header.reason_phrase_hash)
                return Error::from_string_literal("Reason phrase hash mismatch");
        }

        return {};
    }();

    if (result.is_error()) {
        (void)FileSystem::remove(path.string(), FileSystem::RecursionMode::Disallowed);
        return result.release_error();
    }

    auto data_offset = cache_header_size + cache_header.url_size + cache_header.reason_phrase_size;

    return adopt_own(*new CacheEntryReader { disk_cache, index, cache_key, vary_key, move(url), move(path), move(file), fd, cache_header, move(reason_phrase), move(response_headers), data_offset, data_size });
}

CacheEntryReader::CacheEntryReader(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 vary_key, String url, LexicalPath path, NonnullOwnPtr<Core::File> file, int fd, CacheHeader cache_header, Optional<String> reason_phrase, NonnullRefPtr<HeaderList> response_headers, u64 data_offset, u64 data_size)
    : CacheEntry(disk_cache, index, cache_key, vary_key, move(url), move(path), cache_header)
    , m_file(move(file))
    , m_fd(fd)
    , m_reason_phrase(move(reason_phrase))
    , m_response_headers(move(response_headers))
    , m_data_offset(data_offset)
    , m_data_size(data_size)
{
}

void CacheEntryReader::revalidation_succeeded(HeaderList const& response_headers)
{
    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[34;1mCache revalidation succeeded for\033[0m {}", m_url);

    update_header_fields(m_response_headers, response_headers);
    m_index.update_response_headers(m_cache_key, m_vary_key, m_response_headers);

    if (m_revalidation_type != RevalidationType::MustRevalidate)
        close_and_destroy_cache_entry();
}

void CacheEntryReader::revalidation_failed()
{
    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[33;1mCache revalidation failed for\033[0m {}", m_url);

    remove();
    close_and_destroy_cache_entry();
}

void CacheEntryReader::send_to(int socket_fd, Function<void(u64)> on_complete, Function<void(u64)> on_error)
{
    VERIFY(m_socket_fd == -1);
    m_socket_fd = socket_fd;

    m_on_send_complete = move(on_complete);
    m_on_send_error = move(on_error);

    if (m_marked_for_deletion) {
        send_error(Error::from_string_literal("Cache entry has been deleted"));
        return;
    }

    m_socket_write_notifier = Core::Notifier::construct(m_socket_fd, Core::NotificationType::Write);
    m_socket_write_notifier->set_enabled(false);

    m_socket_write_notifier->on_activation = [this]() {
        m_socket_write_notifier->set_enabled(false);
        send_without_blocking();
    };

    send_without_blocking();
}

void CacheEntryReader::send_without_blocking()
{
    if (m_marked_for_deletion) {
        send_error(Error::from_string_literal("Cache entry has been deleted"));
        return;
    }

    auto result = Core::System::transfer_file_through_socket(m_fd, m_socket_fd, m_data_offset + m_bytes_sent, m_data_size - m_bytes_sent);

    if (result.is_error()) {
        if (result.error().code() != EAGAIN && result.error().code() != EWOULDBLOCK)
            send_error(result.release_error());
        else
            m_socket_write_notifier->set_enabled(true);

        return;
    }

    m_bytes_sent += result.value();

    if (m_bytes_sent == m_data_size) {
        send_complete();
        return;
    }

    send_without_blocking();
}

void CacheEntryReader::send_complete()
{
    if (auto result = read_and_validate_footer(); result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mError validating cache entry for\033[0m {}: {}", m_url, result.error());
        remove();

        if (m_on_send_error)
            m_on_send_error(m_bytes_sent);
    } else {
        m_index.update_last_access_time(m_cache_key, m_vary_key);

        if (m_on_send_complete)
            m_on_send_complete(m_bytes_sent);
    }

    close_and_destroy_cache_entry();
}

void CacheEntryReader::send_error(Error error)
{
    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mError transferring cache to socket for\033[0m {}: {}", m_url, error);

    // FIXME: We may not want to actually remove the cache file for all errors. For now, let's assume the file is not
    //        useable at this point and remove it.
    remove();

    if (m_on_send_error)
        m_on_send_error(m_bytes_sent);

    close_and_destroy_cache_entry();
}

ErrorOr<void> CacheEntryReader::read_and_validate_footer()
{
    TRY(m_file->seek(m_data_offset + m_data_size, SeekMode::SetPosition));
    m_cache_footer = TRY(m_file->read_value<CacheFooter>());

    if (m_cache_footer.data_size != m_data_size)
        return Error::from_string_literal("Invalid data size in footer");
    if (m_cache_footer.header_hash != m_cache_header.hash())
        return Error::from_string_literal("Invalid header hash in footer");

    return {};
}

}
