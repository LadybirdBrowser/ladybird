/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <RequestServer/Cache/CacheEntry.h>
#include <RequestServer/Cache/CacheIndex.h>
#include <RequestServer/Cache/DiskCache.h>
#include <RequestServer/Cache/Utilities.h>

namespace RequestServer {

static LexicalPath path_for_cache_key(LexicalPath const& cache_directory, u64 cache_key)
{
    return cache_directory.append(MUST(String::formatted("{:016x}", cache_key)));
}

ErrorOr<CacheHeader> CacheHeader::read_from_stream(Stream& stream)
{
    CacheHeader header;
    header.magic = TRY(stream.read_value<u32>());
    header.version = TRY(stream.read_value<u32>());
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
    TRY(stream.write_value(url_size));
    TRY(stream.write_value(url_hash));
    TRY(stream.write_value(status_code));
    TRY(stream.write_value(reason_phrase_size));
    TRY(stream.write_value(reason_phrase_hash));
    return {};
}

ErrorOr<void> CacheFooter::write_to_stream(Stream& stream) const
{
    TRY(stream.write_value(data_size));
    TRY(stream.write_value(crc32));
    return {};
}

ErrorOr<CacheFooter> CacheFooter::read_from_stream(Stream& stream)
{
    CacheFooter footer;
    footer.data_size = TRY(stream.read_value<u64>());
    footer.crc32 = TRY(stream.read_value<u32>());
    return footer;
}

CacheEntry::CacheEntry(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, LexicalPath path, CacheHeader cache_header)
    : m_disk_cache(disk_cache)
    , m_index(index)
    , m_cache_key(cache_key)
    , m_url(move(url))
    , m_path(move(path))
    , m_cache_header(cache_header)
{
}

void CacheEntry::remove()
{
    (void)FileSystem::remove(m_path.string(), FileSystem::RecursionMode::Disallowed);
    m_index.remove_entry(m_cache_key);
}

void CacheEntry::close_and_destroy_cache_entry()
{
    m_disk_cache.cache_entry_closed({}, *this);
}

ErrorOr<NonnullOwnPtr<CacheEntryWriter>> CacheEntryWriter::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, UnixDateTime request_time)
{
    auto path = path_for_cache_key(disk_cache.cache_directory(), cache_key);

    auto unbuffered_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    auto file = TRY(Core::OutputBufferedFile::create(move(unbuffered_file)));

    CacheHeader cache_header;
    cache_header.url_size = url.byte_count();
    cache_header.url_hash = url.hash();

    return adopt_own(*new CacheEntryWriter { disk_cache, index, cache_key, move(url), move(path), move(file), cache_header, request_time });
}

CacheEntryWriter::CacheEntryWriter(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, LexicalPath path, NonnullOwnPtr<Core::OutputBufferedFile> file, CacheHeader cache_header, UnixDateTime request_time)
    : CacheEntry(disk_cache, index, cache_key, move(url), move(path), cache_header)
    , m_file(move(file))
    , m_request_time(request_time)
    , m_response_time(UnixDateTime::now())
{
}

ErrorOr<void> CacheEntryWriter::write_status_and_reason(u32 status_code, Optional<String> reason_phrase, HTTP::HeaderMap const& response_headers)
{
    if (m_marked_for_deletion) {
        close_and_destroy_cache_entry();
        return Error::from_string_literal("Cache entry has been deleted");
    }

    m_cache_header.status_code = status_code;

    if (reason_phrase.has_value()) {
        m_cache_header.reason_phrase_size = reason_phrase->byte_count();
        m_cache_header.reason_phrase_hash = reason_phrase->hash();
    }

    auto result = [&]() -> ErrorOr<void> {
        if (!is_cacheable(status_code, response_headers))
            return Error::from_string_literal("Response is not cacheable");

        auto freshness_lifetime = calculate_freshness_lifetime(response_headers);
        auto current_age = calculate_age(response_headers, m_request_time, m_response_time);

        // We can cache already-expired responses if there are other cache directives that allow us to revalidate the
        // response on subsequent requests. For example, `Cache-Control: max-age=0, must-revalidate`.
        if (cache_lifetime_status(response_headers, freshness_lifetime, current_age) == CacheLifetimeStatus::Expired)
            return Error::from_string_literal("Response has already expired");

        TRY(m_file->write_value(m_cache_header));
        TRY(m_file->write_until_depleted(m_url));
        if (reason_phrase.has_value())
            TRY(m_file->write_until_depleted(*reason_phrase));

        return {};
    }();

    if (result.is_error()) {
        dbgln("\033[31;1mUnable to write status/reason to cache entry for\033[0m {}: {}", m_url, result.error());

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
        dbgln("\033[31;1mUnable to write data to cache entry for\033[0m {}: {}", m_url, result.error());

        remove();
        close_and_destroy_cache_entry();

        return result.release_error();
    }

    m_cache_footer.data_size += data.size();

    // FIXME: Update the crc.
    return {};
}

ErrorOr<void> CacheEntryWriter::flush(HTTP::HeaderMap response_headers)
{
    ScopeGuard guard { [&]() { close_and_destroy_cache_entry(); } };

    if (m_marked_for_deletion)
        return Error::from_string_literal("Cache entry has been deleted");

    if (auto result = m_file->write_value(m_cache_footer); result.is_error()) {
        dbgln("\033[31;1mUnable to flush cache entry for\033[0m {}: {}", m_url, result.error());
        remove();

        return result.release_error();
    }

    m_index.create_entry(m_cache_key, m_url, move(response_headers), m_cache_footer.data_size, m_request_time, m_response_time);

    dbgln("\033[34;1mFinished caching\033[0m {} ({} bytes)", m_url, m_cache_footer.data_size);
    return {};
}

ErrorOr<NonnullOwnPtr<CacheEntryReader>> CacheEntryReader::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, HTTP::HeaderMap response_headers, u64 data_size)
{
    auto path = path_for_cache_key(disk_cache.cache_directory(), cache_key);

    auto file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Read));
    auto fd = file->fd();

    CacheHeader cache_header;

    String url;
    Optional<String> reason_phrase;

    auto result = [&]() -> ErrorOr<void> {
        cache_header = TRY(file->read_value<CacheHeader>());

        if (cache_header.magic != CacheHeader::CACHE_MAGIC)
            return Error::from_string_literal("Magic value mismatch");
        if (cache_header.version != CACHE_VERSION)
            return Error::from_string_literal("Version mismatch");

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

    auto data_offset = sizeof(CacheHeader) + cache_header.url_size + cache_header.reason_phrase_size;

    return adopt_own(*new CacheEntryReader { disk_cache, index, cache_key, move(url), move(path), move(file), fd, cache_header, move(reason_phrase), move(response_headers), data_offset, data_size });
}

CacheEntryReader::CacheEntryReader(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, LexicalPath path, NonnullOwnPtr<Core::File> file, int fd, CacheHeader cache_header, Optional<String> reason_phrase, HTTP::HeaderMap response_headers, u64 data_offset, u64 data_size)
    : CacheEntry(disk_cache, index, cache_key, move(url), move(path), cache_header)
    , m_file(move(file))
    , m_fd(fd)
    , m_reason_phrase(move(reason_phrase))
    , m_response_headers(move(response_headers))
    , m_data_offset(data_offset)
    , m_data_size(data_size)
{
}

void CacheEntryReader::revalidation_succeeded(HTTP::HeaderMap const& response_headers)
{
    dbgln("\033[34;1mCache revalidation succeeded for\033[0m {}", m_url);

    update_header_fields(m_response_headers, response_headers);
    m_index.update_response_headers(m_cache_key, m_response_headers);
}

void CacheEntryReader::revalidation_failed()
{
    dbgln("\033[33;1mCache revalidation failed for\033[0m {}", m_url);

    remove();
    close_and_destroy_cache_entry();
}

void CacheEntryReader::pipe_to(int pipe_fd, Function<void(u64)> on_complete, Function<void(u64)> on_error)
{
    VERIFY(m_pipe_fd == -1);
    m_pipe_fd = pipe_fd;

    m_on_pipe_complete = move(on_complete);
    m_on_pipe_error = move(on_error);

    if (m_marked_for_deletion) {
        pipe_error(Error::from_string_literal("Cache entry has been deleted"));
        return;
    }

    m_pipe_write_notifier = Core::Notifier::construct(m_pipe_fd, Core::NotificationType::Write);
    m_pipe_write_notifier->set_enabled(false);

    m_pipe_write_notifier->on_activation = [this]() {
        m_pipe_write_notifier->set_enabled(false);
        pipe_without_blocking();
    };

    pipe_without_blocking();
}

void CacheEntryReader::pipe_without_blocking()
{
    if (m_marked_for_deletion) {
        pipe_error(Error::from_string_literal("Cache entry has been deleted"));
        return;
    }

    auto result = Core::System::transfer_file_through_pipe(m_fd, m_pipe_fd, m_data_offset + m_bytes_piped, m_data_size - m_bytes_piped);

    if (result.is_error()) {
        if (result.error().code() != EAGAIN && result.error().code() != EWOULDBLOCK)
            pipe_error(result.release_error());
        else
            m_pipe_write_notifier->set_enabled(true);

        return;
    }

    m_bytes_piped += result.value();

    if (m_bytes_piped == m_data_size) {
        pipe_complete();
        return;
    }

    pipe_without_blocking();
}

void CacheEntryReader::pipe_complete()
{
    if (auto result = read_and_validate_footer(); result.is_error()) {
        dbgln("\033[31;1mError validating cache entry for\033[0m {}: {}", m_url, result.error());
        remove();

        if (m_on_pipe_error)
            m_on_pipe_error(m_bytes_piped);
    } else {
        m_index.update_last_access_time(m_cache_key);

        if (m_on_pipe_complete)
            m_on_pipe_complete(m_bytes_piped);
    }

    close_and_destroy_cache_entry();
}

void CacheEntryReader::pipe_error(Error error)
{
    dbgln("\033[31;1mError transferring cache to pipe for\033[0m {}: {}", m_url, error);

    // FIXME: We may not want to actually remove the cache file for all errors. For now, let's assume the file is not
    //        useable at this point and remove it.
    remove();

    if (m_on_pipe_error)
        m_on_pipe_error(m_bytes_piped);

    close_and_destroy_cache_entry();
}

ErrorOr<void> CacheEntryReader::read_and_validate_footer()
{
    TRY(m_file->seek(m_data_offset + m_data_size, SeekMode::SetPosition));
    m_cache_footer = TRY(m_file->read_value<CacheFooter>());

    if (m_cache_footer.data_size != m_data_size)
        return Error::from_string_literal("Invalid data size in footer");

    // FIXME: Validate the crc.

    return {};
}

}
