/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonArraySerializer.h>
#include <AK/JsonObject.h>
#include <AK/JsonObjectSerializer.h>
#include <AK/JsonValue.h>
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
    header.headers_size = TRY(stream.read_value<u32>());
    header.headers_hash = TRY(stream.read_value<u32>());
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
    TRY(stream.write_value(headers_size));
    TRY(stream.write_value(headers_hash));
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

void CacheEntry::close_and_destory_cache_entry()
{
    m_disk_cache.cache_entry_closed({}, *this);
}

ErrorOr<NonnullOwnPtr<CacheEntryWriter>> CacheEntryWriter::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, u32 status_code, Optional<String> reason_phrase, HTTP::HeaderMap const& headers, UnixDateTime request_time)
{
    auto path = path_for_cache_key(disk_cache.cache_directory(), cache_key);

    auto unbuffered_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    auto file = TRY(Core::OutputBufferedFile::create(move(unbuffered_file)));

    CacheHeader cache_header;

    auto result = [&]() -> ErrorOr<void> {
        StringBuilder builder;
        auto headers_serializer = TRY(JsonArraySerializer<>::try_create(builder));

        for (auto const& header : headers.headers()) {
            if (is_header_exempted_from_storage(header.name))
                continue;

            auto header_serializer = TRY(headers_serializer.add_object());
            TRY(header_serializer.add("name"sv, header.name));
            TRY(header_serializer.add("value"sv, header.value));
            TRY(header_serializer.finish());
        }

        TRY(headers_serializer.finish());

        cache_header.url_size = url.byte_count();
        cache_header.url_hash = url.hash();

        cache_header.status_code = status_code;
        cache_header.reason_phrase_size = reason_phrase.has_value() ? reason_phrase->byte_count() : 0;
        cache_header.reason_phrase_hash = reason_phrase.has_value() ? reason_phrase->hash() : 0;

        auto serialized_headers = builder.string_view();
        cache_header.headers_size = serialized_headers.length();
        cache_header.headers_hash = serialized_headers.hash();

        TRY(file->write_value(cache_header));
        TRY(file->write_until_depleted(url));
        if (reason_phrase.has_value())
            TRY(file->write_until_depleted(*reason_phrase));
        TRY(file->write_until_depleted(serialized_headers));

        return {};
    }();

    if (result.is_error()) {
        (void)FileSystem::remove(path.string(), FileSystem::RecursionMode::Disallowed);
        return result.release_error();
    }

    return adopt_own(*new CacheEntryWriter { disk_cache, index, cache_key, move(url), path, move(file), cache_header, request_time });
}

CacheEntryWriter::CacheEntryWriter(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, LexicalPath path, NonnullOwnPtr<Core::OutputBufferedFile> file, CacheHeader cache_header, UnixDateTime request_time)
    : CacheEntry(disk_cache, index, cache_key, move(url), move(path), cache_header)
    , m_file(move(file))
    , m_request_time(request_time)
    , m_response_time(UnixDateTime::now())
{
}

ErrorOr<void> CacheEntryWriter::write_data(ReadonlyBytes data)
{
    if (m_marked_for_deletion) {
        close_and_destory_cache_entry();
        return Error::from_string_literal("Cache entry has been deleted");
    }

    if (auto result = m_file->write_until_depleted(data); result.is_error()) {
        dbgln("\033[31;1mUnable to write to cache entry for{}\033[0m {}: {}", m_url, result.error());

        remove();
        close_and_destory_cache_entry();

        return result.release_error();
    }

    m_cache_footer.data_size += data.size();

    // FIXME: Update the crc.

    dbgln("\033[36;1mSaved {} bytes for\033[0m {}", data.size(), m_url);
    return {};
}

ErrorOr<void> CacheEntryWriter::flush()
{
    ScopeGuard guard { [&]() { close_and_destory_cache_entry(); } };

    if (m_marked_for_deletion)
        return Error::from_string_literal("Cache entry has been deleted");

    if (auto result = m_file->write_value(m_cache_footer); result.is_error()) {
        dbgln("\033[31;1mUnable to flush cache entry for{}\033[0m {}: {}", m_url, result.error());
        remove();

        return result.release_error();
    }

    m_index.create_entry(m_cache_key, m_url, m_cache_footer.data_size, m_request_time, m_response_time);

    dbgln("\033[34;1mFinished caching\033[0m {} ({} bytes)", m_url, m_cache_footer.data_size);
    return {};
}

ErrorOr<NonnullOwnPtr<CacheEntryReader>> CacheEntryReader::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 data_size)
{
    auto path = path_for_cache_key(disk_cache.cache_directory(), cache_key);

    auto file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Read));
    auto fd = file->fd();

    CacheHeader cache_header;

    String url;
    Optional<String> reason_phrase;
    HTTP::HeaderMap headers;

    auto result = [&]() -> ErrorOr<void> {
        cache_header = TRY(file->read_value<CacheHeader>());

        if (cache_header.magic != CacheHeader::CACHE_MAGIC)
            return Error::from_string_literal("Magic value mismatch");
        if (cache_header.version != CacheHeader::CACHE_VERSION)
            return Error::from_string_literal("Version mismatch");

        url = TRY(String::from_stream(*file, cache_header.url_size));
        if (url.hash() != cache_header.url_hash)
            return Error::from_string_literal("URL hash mismatch");

        if (cache_header.reason_phrase_size != 0) {
            reason_phrase = TRY(String::from_stream(*file, cache_header.reason_phrase_size));
            if (reason_phrase->hash() != cache_header.reason_phrase_hash)
                return Error::from_string_literal("Reason phrase hash mismatch");
        }

        auto serialized_headers = TRY(String::from_stream(*file, cache_header.headers_size));
        if (serialized_headers.hash() != cache_header.headers_hash)
            return Error::from_string_literal("HTTP headers hash mismatch");

        auto json_headers = TRY(JsonValue::from_string(serialized_headers));
        if (!json_headers.is_array())
            return Error::from_string_literal("Expected HTTP headers to be a JSON array");

        TRY(json_headers.as_array().try_for_each([&](JsonValue const& header) -> ErrorOr<void> {
            if (!header.is_object())
                return Error::from_string_literal("Expected headers entry to be a JSON object");

            auto name = header.as_object().get_string("name"sv);
            auto value = header.as_object().get_string("value"sv);

            if (!name.has_value() || !value.has_value())
                return Error::from_string_literal("Missing/invalid data in headers entry");

            headers.set(name->to_byte_string(), value->to_byte_string());
            return {};
        }));

        return {};
    }();

    if (result.is_error()) {
        (void)FileSystem::remove(path.string(), FileSystem::RecursionMode::Disallowed);
        return result.release_error();
    }

    auto data_offset = sizeof(CacheHeader) + cache_header.url_size + cache_header.reason_phrase_size + cache_header.headers_size;

    return adopt_own(*new CacheEntryReader { disk_cache, index, cache_key, move(url), move(path), move(file), fd, cache_header, move(reason_phrase), move(headers), data_offset, data_size });
}

CacheEntryReader::CacheEntryReader(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, LexicalPath path, NonnullOwnPtr<Core::File> file, int fd, CacheHeader cache_header, Optional<String> reason_phrase, HTTP::HeaderMap header_map, u64 data_offset, u64 data_size)
    : CacheEntry(disk_cache, index, cache_key, move(url), move(path), cache_header)
    , m_file(move(file))
    , m_fd(fd)
    , m_reason_phrase(move(reason_phrase))
    , m_headers(move(header_map))
    , m_data_offset(data_offset)
    , m_data_size(data_size)
{
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

    close_and_destory_cache_entry();
}

void CacheEntryReader::pipe_error(Error error)
{
    dbgln("\033[31;1mError transferring cache to pipe for\033[0m {}: {}", m_url, error);

    if (m_on_pipe_error)
        m_on_pipe_error(m_bytes_piped);

    close_and_destory_cache_entry();
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
