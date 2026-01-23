/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibHTTP/Cache/Version.h>
#include <LibHTTP/Forward.h>
#include <LibHTTP/HeaderList.h>

namespace HTTP {

struct CacheHeader {
    static ErrorOr<CacheHeader> read_from_stream(Stream&);
    ErrorOr<void> write_to_stream(Stream&) const;

    u32 hash() const;

    static constexpr auto CACHE_MAGIC = 0xcafef00du;

    u32 magic { CACHE_MAGIC };
    u32 version { CACHE_VERSION };

    u32 key_hash { 0 };

    u32 url_size { 0 };
    u32 url_hash { 0 };

    u32 status_code { 0 };
    u32 reason_phrase_size { 0 };
    u32 reason_phrase_hash { 0 };
};

struct CacheFooter {
    static ErrorOr<CacheFooter> read_from_stream(Stream&);
    ErrorOr<void> write_to_stream(Stream&) const;

    u64 data_size { 0 };
    u32 header_hash { 0 };
};

// A cache entry is an amalgamation of all information needed to reconstruct HTTP responses. It is created once we have
// received the response headers for a request. The body is streamed into the entry as it is received. The cache format
// on disk is:
//
//     [CacheHeader][URL][ReasonPhrase][Data][CacheFooter]
class CacheEntry {
public:
    virtual ~CacheEntry() = default;

    u64 cache_key() const { return m_cache_key; }
    u64 vary_key() const { return m_vary_key; }

    void remove();

    void mark_for_deletion(Badge<DiskCache>) { m_marked_for_deletion = true; }

protected:
    CacheEntry(DiskCache&, CacheIndex&, u64 cache_key, u64 vary_key, String url, Optional<LexicalPath>, CacheHeader);

    void close_and_destroy_cache_entry();

    DiskCache& m_disk_cache;
    CacheIndex& m_index;

    u64 m_cache_key { 0 };
    u64 m_vary_key { 0 };

    String m_url;
    Optional<LexicalPath> m_path;

    CacheHeader m_cache_header;
    CacheFooter m_cache_footer;

    bool m_marked_for_deletion { false };
};

class CacheEntryWriter final : public CacheEntry {
public:
    static ErrorOr<NonnullOwnPtr<CacheEntryWriter>> create(DiskCache&, CacheIndex&, u64 cache_key, String url, UnixDateTime request_time, AK::Duration current_time_offset_for_testing);
    virtual ~CacheEntryWriter() override = default;

    ErrorOr<void> write_status_and_reason(u32 status_code, Optional<String> reason_phrase, HeaderList const& request_headers, HeaderList const& response_headers);
    ErrorOr<void> write_data(ReadonlyBytes);
    ErrorOr<void> flush(NonnullRefPtr<HeaderList> request_headers, NonnullRefPtr<HeaderList> response_headers);
    void remove_incomplete_entry();

private:
    CacheEntryWriter(DiskCache&, CacheIndex&, u64 cache_key, String url, CacheHeader, UnixDateTime request_time, AK::Duration current_time_offset_for_testing);

    OwnPtr<Core::OutputBufferedFile> m_file;

    UnixDateTime m_request_time;
    UnixDateTime m_response_time;

    AK::Duration m_current_time_offset_for_testing;
};

class CacheEntryReader final : public CacheEntry {
public:
    static ErrorOr<NonnullOwnPtr<CacheEntryReader>> create(DiskCache&, CacheIndex&, u64 cache_key, u64 vary_key, NonnullRefPtr<HeaderList>, u64 data_size);
    virtual ~CacheEntryReader() override = default;

    enum class RevalidationType {
        None,
        MustRevalidate,
        StaleWhileRevalidate,
    };
    RevalidationType revalidation_type() const { return m_revalidation_type; }
    void set_revalidation_type(RevalidationType revalidation_type) { m_revalidation_type = revalidation_type; }

    void revalidation_succeeded(HeaderList const&);
    void revalidation_failed();

    void send_to(int socket_fd, Function<void(u64 bytes_sent)> on_complete, Function<void(u64 bytes_sent)> on_error);

    u32 status_code() const { return m_cache_header.status_code; }
    Optional<String> const& reason_phrase() const { return m_reason_phrase; }
    HeaderList& response_headers() { return m_response_headers; }
    HeaderList const& response_headers() const { return m_response_headers; }

private:
    CacheEntryReader(DiskCache&, CacheIndex&, u64 cache_key, u64 vary_key, String url, LexicalPath, NonnullOwnPtr<Core::File>, int fd, CacheHeader, Optional<String> reason_phrase, NonnullRefPtr<HeaderList>, u64 data_offset, u64 data_size);

    void send_without_blocking();
    void send_complete();
    void send_error(Error);

    ErrorOr<void> read_and_validate_footer();

    NonnullOwnPtr<Core::File> m_file;
    int m_fd { -1 };

    RefPtr<Core::Notifier> m_socket_write_notifier;
    int m_socket_fd { -1 };

    Function<void(u64)> m_on_send_complete;
    Function<void(u64)> m_on_send_error;
    u64 m_bytes_sent { 0 };

    Optional<String> m_reason_phrase;
    NonnullRefPtr<HeaderList> m_response_headers;

    RevalidationType m_revalidation_type { RevalidationType::None };

    u64 const m_data_offset { 0 };
    u64 const m_data_size { 0 };
};

}
