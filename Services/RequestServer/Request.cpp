/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/HashMap.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibCore/Notifier.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/Status.h>
#include <LibTextCodec/Decoder.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Request.h>
#include <RequestServer/Resolver.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

extern OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

static long s_connect_timeout_seconds = 90L;

static void log_network_activity(URL::URL const& url, ByteString const& method, void* curl_easy_handle, int curl_result_code, bool is_revalidation, RequestType type)
{
    if constexpr (!REQUESTSERVER_WIRE_DEBUG)
        return;
    if (!curl_easy_handle)
        return;

    auto get_off = [&](auto option) {
        curl_off_t value = 0;
        (void)curl_easy_getinfo(curl_easy_handle, option, &value);
        return value;
    };
    auto get_long = [&](auto option) {
        long value = 0;
        (void)curl_easy_getinfo(curl_easy_handle, option, &value);
        return value;
    };

    long http_status = get_long(CURLINFO_RESPONSE_CODE);
    long http_version = get_long(CURLINFO_HTTP_VERSION);

    auto queue_us = get_off(CURLINFO_QUEUE_TIME_T);
    auto namelookup_us = get_off(CURLINFO_NAMELOOKUP_TIME_T);
    auto connect_us = get_off(CURLINFO_CONNECT_TIME_T);
    auto appconnect_us = get_off(CURLINFO_APPCONNECT_TIME_T);
    auto pretransfer_us = get_off(CURLINFO_PRETRANSFER_TIME_T);
    auto starttransfer_us = get_off(CURLINFO_STARTTRANSFER_TIME_T);
    auto total_us = get_off(CURLINFO_TOTAL_TIME_T);
    auto bytes_downloaded = get_off(CURLINFO_SIZE_DOWNLOAD_T);
    auto bytes_uploaded = get_off(CURLINFO_SIZE_UPLOAD_T);
    auto download_speed_bps = get_off(CURLINFO_SPEED_DOWNLOAD_T);

    // libcurl phase timings are cumulative from t=0, but skipped phases (e.g. DNS/TCP/TLS on a
    // reused connection) are reported as 0, breaking the monotonic ordering. Clamp each marker
    // to the previous one so skipped phases yield a 0-length delta instead of double-counting.
    auto clamp = [](curl_off_t marker, curl_off_t previous) { return marker > previous ? marker : previous; };
    auto m_queue = queue_us;
    auto m_namelookup = clamp(namelookup_us, m_queue);
    auto m_connect = clamp(connect_us, m_namelookup);
    auto m_appconnect = clamp(appconnect_us, m_connect);
    auto m_pretransfer = clamp(pretransfer_us, m_appconnect);
    auto m_starttransfer = clamp(starttransfer_us, m_pretransfer);
    auto m_total = clamp(total_us, m_starttransfer);

    auto us_to_ms = [](curl_off_t us) { return static_cast<double>(us) / 1000.0; };
    auto queue_ms = us_to_ms(m_queue);
    auto dns_ms = us_to_ms(m_namelookup - m_queue);
    auto tcp_ms = us_to_ms(m_connect - m_namelookup);
    auto tls_ms = us_to_ms(m_appconnect - m_connect);
    auto request_ms = us_to_ms(m_pretransfer - m_appconnect);
    auto wait_ms = us_to_ms(m_starttransfer - m_pretransfer);
    auto body_ms = us_to_ms(m_total - m_starttransfer);
    auto total_ms = us_to_ms(m_total);

    auto kib = [](curl_off_t bytes) { return static_cast<double>(bytes) / 1024.0; };

    StringView http_version_str = "HTTP/?"sv;
    switch (http_version) {
    case CURL_HTTP_VERSION_1_0:
        http_version_str = "HTTP/1.0"sv;
        break;
    case CURL_HTTP_VERSION_1_1:
        http_version_str = "HTTP/1.1"sv;
        break;
    case CURL_HTTP_VERSION_2_0:
        http_version_str = "HTTP/2"sv;
        break;
    case CURL_HTTP_VERSION_3:
        http_version_str = "HTTP/3"sv;
        break;
    default:
        break;
    }

    StringView kind;
    if (curl_result_code != CURLE_OK)
        kind = "FAIL"sv;
    else if (is_revalidation && http_status == 304)
        kind = "REVAL-304"sv;
    else if (is_revalidation)
        kind = "REVAL-FULL"sv;
    else
        kind = "DOWNLOAD"sv;

    StringView background = type == RequestType::BackgroundRevalidation ? " [bg]"sv : ""sv;

    if (curl_result_code != CURLE_OK) {
        char const* err = curl_easy_strerror(static_cast<CURLcode>(curl_result_code));
        dbgln_if(REQUESTSERVER_WIRE_DEBUG, "RequestServer wire: {}{} {} {} -> error: {} (after {:.1} ms, wire {:.1} KiB)",
            kind, background, method, url, err, total_ms, kib(bytes_downloaded));
        return;
    }

    auto wire_kibps_during_body = body_ms > 0.0
        ? kib(bytes_downloaded) / (body_ms / 1000.0)
        : 0.0;

    dbgln_if(REQUESTSERVER_WIRE_DEBUG, "RequestServer wire: {}{} {} {} {} -> {} | wire {:.1} KiB sent {:.1} KiB | total {:.1} ms = queue {:.1} + dns {:.1} + tcp {:.1} + tls {:.1} + req {:.1} + wait {:.1} + body {:.1} | wire {:.1} KiB/s avg, {:.1} KiB/s during body",
        kind, background, method, url, http_version_str, http_status,
        kib(bytes_downloaded), kib(bytes_uploaded),
        total_ms, queue_ms, dns_ms, tcp_ms, tls_ms, request_ms, wait_ms, body_ms,
        kib(download_speed_bps), wire_kibps_during_body);
}

struct WireStats {
    // Decoded-side (sampled in on_data_received, after curl decompresses)
    Optional<MonotonicTime> first_chunk_at;
    Optional<MonotonicTime> last_chunk_at;
    u64 chunk_count { 0 };
    u64 total_decoded_bytes { 0 };
    u64 min_chunk_bytes { NumericLimits<u64>::max() };
    u64 max_chunk_bytes { 0 };
    AK::Duration sum_inter_chunk_gaps;
    AK::Duration max_inter_chunk_gap;
    u64 stall_count_100ms { 0 };
    AK::Duration max_stall_started_at;
    u64 bytes_at_max_stall { 0 };

    // Wire-side (sampled in CURLOPT_XFERINFOFUNCTION, before decompression)
    Optional<MonotonicTime> first_wire_byte_at;
    Optional<MonotonicTime> last_wire_byte_at;
    curl_off_t last_wire_dlnow { 0 };
    AK::Duration max_wire_gap;
    AK::Duration max_wire_stall_started_at;
    curl_off_t wire_bytes_at_max_stall { 0 };
    u64 wire_stall_count_100ms { 0 };

    // Internal-pipeline lifecycle (set by state-machine handlers).
    // Lets us see how much wall time we burned in our own code paths
    // (cache lookup, our DNS resolver, cookie IPC, curl setup) before
    // libcurl ever saw the request.
    Optional<MonotonicTime> created_at;
    Optional<MonotonicTime> dns_started_at;
    Optional<MonotonicTime> dns_completed_at;
    Optional<MonotonicTime> cookie_started_at;
    Optional<MonotonicTime> cookie_completed_at;
    Optional<MonotonicTime> curl_added_at;
    Optional<MonotonicTime> complete_observed_at;

    // WebContent-side back-pressure on our outgoing pipe. Updated by
    // write_queued_bytes_without_blocking when the pipe returns EAGAIN
    // (WebContent isn't draining fast enough — typically because its main
    // thread is busy parsing, running JS, etc.). `current_window_started`
    // is set on entry to a back-pressure window and cleared when we resume
    // writing successfully.
    Optional<MonotonicTime> current_pressure_window_started;
    AK::Duration total_pipe_back_pressure;
    u64 pipe_back_pressure_events { 0 };
    u64 max_buffered_bytes { 0 };
};

static HashMap<Request const*, WireStats>& wire_stats()
{
    static HashMap<Request const*, WireStats> map;
    return map;
}

static void record_chunk(Request const* request, size_t bytes)
{
    if constexpr (!REQUESTSERVER_WIRE_DEBUG)
        return;
    auto now = MonotonicTime::now();
    auto& stats = wire_stats().ensure(request);

    if (stats.chunk_count == 0) {
        stats.first_chunk_at = now;
    } else {
        auto gap = now - *stats.last_chunk_at;
        stats.sum_inter_chunk_gaps = stats.sum_inter_chunk_gaps + gap;
        if (gap > stats.max_inter_chunk_gap) {
            stats.max_inter_chunk_gap = gap;
            stats.max_stall_started_at = *stats.last_chunk_at - *stats.first_chunk_at;
            stats.bytes_at_max_stall = stats.total_decoded_bytes;
        }
        if (gap > AK::Duration::from_milliseconds(100))
            stats.stall_count_100ms += 1;
    }

    stats.last_chunk_at = now;
    stats.chunk_count += 1;
    stats.total_decoded_bytes += bytes;
    if (bytes < stats.min_chunk_bytes)
        stats.min_chunk_bytes = bytes;
    if (bytes > stats.max_chunk_bytes)
        stats.max_chunk_bytes = bytes;
}

[[maybe_unused]] static int on_xferinfo(void* user_data, curl_off_t /*dltotal*/, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    if constexpr (!REQUESTSERVER_WIRE_DEBUG)
        return 0;
    auto* request = static_cast<Request const*>(user_data);
    if (dlnow <= 0)
        return 0;

    auto& stats = wire_stats().ensure(request);
    if (dlnow == stats.last_wire_dlnow)
        return 0;

    auto now = MonotonicTime::now();
    if (!stats.first_wire_byte_at.has_value())
        stats.first_wire_byte_at = now;
    if (stats.last_wire_byte_at.has_value()) {
        auto gap = now - *stats.last_wire_byte_at;
        if (gap > stats.max_wire_gap) {
            stats.max_wire_gap = gap;
            stats.max_wire_stall_started_at = *stats.last_wire_byte_at - *stats.first_wire_byte_at;
            stats.wire_bytes_at_max_stall = stats.last_wire_dlnow;
        }
        if (gap > AK::Duration::from_milliseconds(100))
            stats.wire_stall_count_100ms += 1;
    }
    stats.last_wire_byte_at = now;
    stats.last_wire_dlnow = dlnow;
    return 0;
}

static void log_chunk_stats(Request const* request)
{
    if constexpr (!REQUESTSERVER_WIRE_DEBUG)
        return;
    auto it = wire_stats().find(request);
    if (it == wire_stats().end())
        return;
    auto const& s = it->value;
    if (s.chunk_count == 0)
        return;

    auto kib = [](u64 b) { return static_cast<double>(b) / 1024.0; };
    auto kib_off = [](curl_off_t b) { return static_cast<double>(b) / 1024.0; };
    auto decoded_span = (s.last_chunk_at.has_value() && s.first_chunk_at.has_value())
        ? *s.last_chunk_at - *s.first_chunk_at
        : AK::Duration {};
    auto decoded_span_ms = decoded_span.to_milliseconds();
    auto avg_gap_ms = s.chunk_count > 1
        ? static_cast<double>(s.sum_inter_chunk_gaps.to_microseconds()) / 1000.0 / static_cast<double>(s.chunk_count - 1)
        : 0.0;
    auto avg_chunk = s.total_decoded_bytes / s.chunk_count;
    auto decoded_kibps = decoded_span_ms > 0
        ? kib(s.total_decoded_bytes) / (static_cast<double>(decoded_span_ms) / 1000.0)
        : 0.0;

    dbgln_if(REQUESTSERVER_WIRE_DEBUG, "RequestServer wire+:  decoded chunks={} bytes={:.1} KiB span={} ms thru={:.1} KiB/s | gap avg={:.1} ms max={} ms stalls(>100ms)={} | worst gap began at +{} ms after {:.1} KiB decoded | chunk bytes avg={} min={} max={}",
        s.chunk_count, kib(s.total_decoded_bytes), decoded_span_ms, decoded_kibps,
        avg_gap_ms, s.max_inter_chunk_gap.to_milliseconds(), s.stall_count_100ms,
        s.max_stall_started_at.to_milliseconds(), kib(s.bytes_at_max_stall),
        avg_chunk, s.min_chunk_bytes, s.max_chunk_bytes);

    if (s.first_wire_byte_at.has_value() && s.last_wire_byte_at.has_value()) {
        auto wire_span = *s.last_wire_byte_at - *s.first_wire_byte_at;
        auto wire_span_ms = wire_span.to_milliseconds();
        auto wire_kibps = wire_span_ms > 0
            ? kib_off(s.last_wire_dlnow) / (static_cast<double>(wire_span_ms) / 1000.0)
            : 0.0;
        auto compression_ratio = s.last_wire_dlnow > 0
            ? static_cast<double>(s.total_decoded_bytes) / static_cast<double>(s.last_wire_dlnow)
            : 0.0;
        dbgln_if(REQUESTSERVER_WIRE_DEBUG, "RequestServer wire++: wire bytes={:.1} KiB span={} ms thru={:.1} KiB/s | wire max gap={} ms stalls(>100ms)={} | worst wire gap began at +{} ms after {:.1} KiB on the wire | compression={:.2}x",
            kib_off(s.last_wire_dlnow), wire_span_ms, wire_kibps,
            s.max_wire_gap.to_milliseconds(), s.wire_stall_count_100ms,
            s.max_wire_stall_started_at.to_milliseconds(), kib_off(s.wire_bytes_at_max_stall),
            compression_ratio);
    }

    // wire^: pre-network time spent inside RequestServer (cache + our DNS resolver + cookie IPC
    // + curl setup), and the gap between the wire going quiet and us actually emitting this log
    // entry. Useful to see if a long "total" was actually our pipeline rather than the network.
    if (s.created_at.has_value()) {
        auto delta_ms = [](Optional<MonotonicTime> const& a, Optional<MonotonicTime> const& b) -> i64 {
            if (!a.has_value() || !b.has_value())
                return -1;
            return (*b - *a).to_milliseconds();
        };

        // Cache + Init + WaitForCache: from creation to start of DNS lookup.
        auto pre_dns_ms = delta_ms(s.created_at, s.dns_started_at);
        // Our DNS resolver (note: distinct from libcurl's `dns` field, which is 0 because we
        // pre-resolve and pass via CURLOPT_RESOLVE).
        auto our_dns_ms = delta_ms(s.dns_started_at, s.dns_completed_at);
        // Cookie IPC round-trip to the UI process.
        auto cookie_ms = delta_ms(s.cookie_started_at, s.cookie_completed_at);
        // Time from the last completed pre-network step to curl_multi_add_handle.
        auto curl_setup_ms = [&]() -> i64 {
            Optional<MonotonicTime> last_pre_curl;
            if (s.cookie_completed_at.has_value())
                last_pre_curl = s.cookie_completed_at;
            else if (s.dns_completed_at.has_value())
                last_pre_curl = s.dns_completed_at;
            else
                last_pre_curl = s.created_at;
            return delta_ms(last_pre_curl, s.curl_added_at);
        }();
        auto pre_curl_total_ms = delta_ms(s.created_at, s.curl_added_at);

        // Drain delay: time between the last byte arriving and check_active_requests draining
        // the completion. Non-zero would mean we noticed completion later than curl did.
        Optional<MonotonicTime> last_activity;
        if (s.last_wire_byte_at.has_value())
            last_activity = s.last_wire_byte_at;
        else if (s.last_chunk_at.has_value())
            last_activity = s.last_chunk_at;
        auto drain_delay_ms = delta_ms(last_activity, s.complete_observed_at);

        auto fmt_ms = [](i64 ms) -> ByteString {
            return ms < 0 ? ByteString { "-" } : ByteString::formatted("{}", ms);
        };

        ByteString back_pressure_summary;
        if (s.pipe_back_pressure_events > 0) {
            back_pressure_summary = ByteString::formatted(
                " | pipe back-pressure events={} total={} ms peak-buffered={} bytes",
                s.pipe_back_pressure_events,
                s.total_pipe_back_pressure.to_milliseconds(),
                s.max_buffered_bytes);
        }

        dbgln_if(REQUESTSERVER_WIRE_DEBUG, "RequestServer wire^:  internal pre-curl={} ms = cache+init {} + our-dns {} + cookie {} + curl-setup {} | drain delay {} ms{}",
            fmt_ms(pre_curl_total_ms), fmt_ms(pre_dns_ms), fmt_ms(our_dns_ms),
            fmt_ms(cookie_ms), fmt_ms(curl_setup_ms), fmt_ms(drain_delay_ms),
            back_pressure_summary);
    }
}

static void mark_lifecycle_event(Request const* request, Optional<MonotonicTime> WireStats::* field)
{
    if constexpr (!REQUESTSERVER_WIRE_DEBUG)
        return;
    wire_stats().ensure(request).*field = MonotonicTime::now();
}

NonnullOwnPtr<Request> Request::fetch(
    u64 request_id,
    Optional<HTTP::DiskCache&> disk_cache,
    HTTP::CacheMode cache_mode,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    NonnullRefPtr<HTTP::HeaderList> request_headers,
    ByteBuffer request_body,
    HTTP::Cookie::IncludeCredentials include_credentials,
    ByteString alt_svc_cache_path,
    Core::ProxyData proxy_data)
{
    auto request = adopt_own(*new Request { request_id, RequestType::Fetch, disk_cache, cache_mode, client, curl_multi, resolver, move(url), move(method), move(request_headers), move(request_body), include_credentials, move(alt_svc_cache_path), proxy_data });
    request->process();

    return request;
}

NonnullOwnPtr<Request> Request::connect(
    u64 request_id,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    CacheLevel cache_level)
{
    auto request = adopt_own(*new Request { request_id, client, curl_multi, resolver, move(url) });
    request->m_connect_cache_level = cache_level;
    request->transition_to_state(State::DNSLookup);
    return request;
}

NonnullOwnPtr<Request> Request::revalidate(
    u64 request_id,
    Optional<HTTP::DiskCache&> disk_cache,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    NonnullRefPtr<HTTP::HeaderList> request_headers,
    ByteBuffer request_body,
    HTTP::Cookie::IncludeCredentials include_credentials,
    ByteString alt_svc_cache_path,
    Core::ProxyData proxy_data)
{
    auto request = adopt_own(*new Request { request_id, RequestType::BackgroundRevalidation, disk_cache, HTTP::CacheMode::Default, client, curl_multi, resolver, move(url), move(method), move(request_headers), move(request_body), include_credentials, move(alt_svc_cache_path), proxy_data });
    request->process();

    return request;
}

Request::Request(
    u64 request_id,
    RequestType type,
    Optional<HTTP::DiskCache&> disk_cache,
    HTTP::CacheMode cache_mode,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    NonnullRefPtr<HTTP::HeaderList> request_headers,
    ByteBuffer request_body,
    HTTP::Cookie::IncludeCredentials include_credentials,
    ByteString alt_svc_cache_path,
    Core::ProxyData proxy_data)
    : m_request_id(request_id)
    , m_type(type)
    , m_disk_cache(disk_cache)
    , m_cache_mode(cache_mode)
    , m_client(client)
    , m_curl_multi_handle(curl_multi)
    , m_resolver(resolver)
    , m_url(move(url))
    , m_method(move(method))
    , m_request_headers(move(request_headers))
    , m_request_body(move(request_body))
    , m_include_credentials(include_credentials)
    , m_alt_svc_cache_path(move(alt_svc_cache_path))
    , m_proxy_data(proxy_data)
    , m_response_headers(HTTP::HeaderList::create())
{
    if constexpr (REQUESTSERVER_WIRE_DEBUG)
        wire_stats().ensure(this).created_at = MonotonicTime::now();
}

Request::Request(
    u64 request_id,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url)
    : m_request_id(request_id)
    , m_type(RequestType::Connect)
    , m_client(client)
    , m_curl_multi_handle(curl_multi)
    , m_resolver(resolver)
    , m_url(move(url))
    , m_request_headers(HTTP::HeaderList::create())
    , m_response_headers(HTTP::HeaderList::create())
{
    if constexpr (REQUESTSERVER_WIRE_DEBUG)
        wire_stats().ensure(this).created_at = MonotonicTime::now();
}

Request::~Request()
{
    if (!m_response_buffer.is_eof())
        dbgln("Warning: Request destroyed with buffered data (it's likely that the client disappeared or the request was cancelled)");

    if (m_curl_easy_handle) {
        auto result = curl_multi_remove_handle(m_curl_multi_handle, m_curl_easy_handle);
        VERIFY(result == CURLM_OK);

        curl_easy_cleanup(m_curl_easy_handle);
    }

    for (auto* string_list : m_curl_string_lists)
        curl_slist_free_all(string_list);

    if (m_cache_entry_writer.has_value()) {
        if (m_state == State::Complete)
            (void)m_cache_entry_writer->flush(m_request_headers, m_response_headers);
        else
            m_cache_entry_writer->remove_incomplete_entry();
    }

    if constexpr (REQUESTSERVER_WIRE_DEBUG)
        wire_stats().remove(this);
}

void Request::notify_request_unblocked(Badge<HTTP::DiskCache>)
{
    // FIXME: We may want a timer to limit how long we are waiting for a request before proceeding with a network
    //        request that skips the disk cache.
    transition_to_state(State::Init);
}

void Request::notify_retrieved_http_cookie(Badge<ConnectionFromClient>, StringView cookie)
{
    mark_lifecycle_event(this, &WireStats::cookie_completed_at);

    if (!cookie.is_empty()) {
        auto header = HTTP::Header::isomorphic_encode("Cookie"sv, cookie);
        m_request_headers->append(move(header));
    }

    transition_to_state(State::Fetch);
}

void Request::notify_fetch_complete(Badge<ConnectionFromClient>, int result_code)
{
    mark_lifecycle_event(this, &WireStats::complete_observed_at);

    if (m_type == RequestType::Fetch || m_type == RequestType::BackgroundRevalidation) {
        log_network_activity(m_url, m_method, m_curl_easy_handle, result_code, is_revalidation_request(), m_type);
        log_chunk_stats(this);
    }

    if (is_revalidation_request()) {
        if (acquire_status_code() == 304) {
            if (m_type == RequestType::BackgroundRevalidation && m_disk_cache->mode() == HTTP::DiskCache::Mode::Testing)
                m_response_headers->set({ HTTP::TEST_CACHE_REVALIDATION_STATUS_HEADER, "fresh"sv });

            m_cache_entry_reader->revalidation_succeeded(m_response_headers);
            transition_to_state(m_type == RequestType::Fetch ? State::ReadCache : State::Complete);
            return;
        }

        if (revalidation_failed().is_error())
            return;

        transfer_headers_to_client_if_needed();
    }

    m_curl_result_code = result_code;

    if (m_response_buffer.is_eof())
        transition_to_state(State::Complete);
}

void Request::transition_to_state(State state)
{
    dbgln_if(REQUESTSERVER_DEBUG, "Request::Transition[{}]: {} -> {} ({} {})", m_request_id, state_name(m_state), state_name(state), m_method, m_url);
    m_state = state;
    process();
}

void Request::process()
{
    switch (m_state) {
    case State::Init:
        handle_initial_state();
        break;
    case State::ReadCache:
        handle_read_cache_state();
        break;
    case State::WaitForCache:
        // Do nothing; we are waiting for the disk cache to notify us to proceed.
        break;
    case State::FailedCacheOnly:
        handle_failed_cache_only_state();
        break;
    case State::ServeSubstitution:
        handle_serve_substitution_state();
        break;
    case State::DNSLookup:
        handle_dns_lookup_state();
        break;
    case State::RetrieveCookie:
        handle_retrieve_cookie_state();
        break;
    case State::Connect:
        handle_connect_state();
        break;
    case State::Fetch:
        handle_fetch_state();
        break;
    case State::Complete:
        handle_complete_state();
        break;
    case State::Error:
        handle_error_state();
        break;
    }
}

void Request::handle_initial_state()
{
    // Check for resource substitution before anything else.
    if (g_resource_substitution_map) {
        if (g_resource_substitution_map->lookup(m_url).has_value()) {
            transition_to_state(State::ServeSubstitution);
            return;
        }
    }

    if (m_cache_mode == HTTP::CacheMode::NoStore) {
        m_cache_status = HTTP::CacheRequest::CacheStatus::NotCached;
    } else if (m_disk_cache.has_value()) {
        auto open_mode = m_type == RequestType::BackgroundRevalidation
            ? HTTP::DiskCache::OpenMode::Revalidate
            : HTTP::DiskCache::OpenMode::Read;

        m_disk_cache->open_entry(*this, m_url, m_method, m_request_headers, m_cache_mode, open_mode)
            .visit(
                [&](Optional<HTTP::CacheEntryReader&> cache_entry_reader) {
                    m_cache_entry_reader = cache_entry_reader;

                    if (m_cache_entry_reader.has_value()) {
                        if (m_cache_entry_reader->revalidation_type() == HTTP::CacheEntryReader::RevalidationType::StaleWhileRevalidate)
                            m_client.start_revalidation_request({}, m_method, m_url, m_request_headers, m_request_body, m_include_credentials, m_proxy_data);

                        if (is_revalidation_request())
                            transition_to_state(State::DNSLookup);
                        else
                            transition_to_state(State::ReadCache);
                    } else if (m_type == RequestType::BackgroundRevalidation) {
                        // If we were not able to open a cache entry reader for revalidation requests, there's no point
                        // in issuing a request over the network.
                        transition_to_state(State::Complete);
                    }
                },
                [&](HTTP::DiskCache::CacheHasOpenEntry) {
                    // If an existing entry is open for writing, we must wait for it to complete.
                    transition_to_state(State::WaitForCache);
                });

        if (m_state != State::Init)
            return;

        if (is_cache_only_request()) {
            transition_to_state(State::FailedCacheOnly);
            return;
        }

        m_disk_cache->create_entry(*this, m_url, m_method, m_request_headers, m_request_start_time)
            .visit(
                [&](Optional<HTTP::CacheEntryWriter&> cache_entry_writer) {
                    m_cache_entry_writer = cache_entry_writer;

                    if (!m_cache_entry_writer.has_value())
                        m_cache_status = CacheStatus::NotCached;
                },
                [&](HTTP::DiskCache::CacheHasOpenEntry) {
                    // If an existing entry is open for reading or writing, we must wait for it to complete. An entry being
                    // open for reading is a rare case, but may occur if a cached response expired between the existing
                    // entry's cache validation and the attempted reader validation when this request was created.
                    transition_to_state(State::WaitForCache);
                });

        if (m_state != State::Init)
            return;
    }

    transition_to_state(State::DNSLookup);
}

void Request::handle_read_cache_state()
{
    m_status_code = m_cache_entry_reader->status_code();
    m_reason_phrase = m_cache_entry_reader->reason_phrase();
    m_response_headers = m_cache_entry_reader->response_headers();
    m_cache_status = CacheStatus::ReadFromCache;

    if (inform_client_request_started().is_error())
        return;
    transfer_headers_to_client_if_needed();

    m_cache_entry_reader->send_to(
        m_client_request_pipe->writer_fd(),
        weak_callback(*this, [](auto& self, auto bytes_sent) {
            self.m_bytes_transferred_to_client = bytes_sent;
            self.m_curl_result_code = CURLE_OK;

            self.transition_to_state(State::Complete);
        }),
        weak_callback(*this, [](auto& self, auto bytes_sent) {
            self.m_bytes_transferred_to_client = bytes_sent;
            self.m_network_error = Requests::NetworkError::CacheReadFailed;

            self.transition_to_state(State::Error);
        }));
}

void Request::handle_failed_cache_only_state()
{
    if (m_cache_mode == HTTP::CacheMode::OnlyIfCached) {
        // NB: Contrary to the HTTP Caching RFC, the Fetch API expects a network error. See:
        // https://github.com/web-platform-tests/wpt/issues/8032
        transition_to_state(State::Error);
        return;
    }

    // https://httpwg.org/specs/rfc9111.html#cache-request-directive.only-if-cached
    // The only-if-cached request directive indicates that the client only wishes to obtain a stored response. Caches
    // that honor this request directive SHOULD, upon receiving it, respond with either a stored response consistent
    // with the other constraints of the request or a 504 (Gateway Timeout) status code.
    m_status_code = 504;
    m_reason_phrase = MUST(String::from_utf8(HTTP::reason_phrase_for_code(*m_status_code)));

    // NB: There is a privacy concern around allowing any origin to determine the cache state of other origins using
    //     this Cache-Control directive. The Fetch API will prevent { cache: "only-if-cached" } requests that do not
    //     pass a same-origin test. We mimic this here with the Access-Control-Allow-Origin response header.
    m_response_headers->set({ "Access-Control-Allow-Origin"sv, m_url.origin().serialize().to_byte_string() });

    if (inform_client_request_started().is_error())
        return;
    transfer_headers_to_client_if_needed();

    m_curl_result_code = CURLE_OK;
    transition_to_state(State::Complete);
}

void Request::handle_serve_substitution_state()
{
    auto substitution = g_resource_substitution_map->lookup(m_url);
    VERIFY(substitution.has_value());

    dbgln("Request: Substituting '{}' with local file '{}'", m_url.serialize(), substitution->file_path);

    auto file = Core::File::open(substitution->file_path, Core::File::OpenMode::Read);
    if (file.is_error()) {
        dbgln("Request::handle_serve_substitution_state: Failed to open file '{}': {}", substitution->file_path, file.error());
        m_network_error = Requests::NetworkError::Unknown;
        transition_to_state(State::Error);
        return;
    }

    auto content = file.value()->read_until_eof();
    if (content.is_error()) {
        dbgln("Request::handle_serve_substitution_state: Failed to read file '{}': {}", substitution->file_path, content.error());
        m_network_error = Requests::NetworkError::Unknown;
        transition_to_state(State::Error);
        return;
    }

    m_status_code = substitution->status_code;
    m_reason_phrase = MUST(String::from_utf8(HTTP::reason_phrase_for_code(*m_status_code)));

    // Determine content type: use override if provided, otherwise guess from filename.
    StringView content_type;
    if (substitution->content_type.has_value())
        content_type = *substitution->content_type;
    else
        content_type = Core::guess_mime_type_based_on_filename(substitution->file_path);

    m_response_headers->append({ "Content-Type"sv, ByteString { content_type } });
    m_response_headers->append({ "Content-Length"sv, ByteString::number(content.value().size()) });
    m_response_headers->append({ "Access-Control-Allow-Origin"sv, "*"sv });

    if (inform_client_request_started().is_error())
        return;
    transfer_headers_to_client_if_needed();

    auto write_result = m_response_buffer.write_some(content.value());
    if (write_result.is_error()) {
        dbgln("Request::handle_serve_substitution_state: Failed to write content to response buffer: {}", write_result.error());
        m_network_error = Requests::NetworkError::Unknown;
        transition_to_state(State::Error);
        return;
    }

    m_curl_result_code = CURLE_OK;

    if (write_queued_bytes_without_blocking().is_error())
        transition_to_state(State::Error);
}

void Request::handle_dns_lookup_state()
{
    auto host = m_url.serialized_host().to_byte_string();
    auto const& dns_info = DNSInfo::the();

    mark_lifecycle_event(this, &WireStats::dns_started_at);

    m_resolver->dns.lookup(host, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA }, { .validate_dnssec_locally = dns_info.validate_dnssec_locally })
        ->when_rejected(weak_callback(*this, [host](auto& self, auto const& error) {
            mark_lifecycle_event(&self, &WireStats::dns_completed_at);
            dbgln("Request::handle_dns_lookup_state: DNS lookup failed for '{}': {}", host, error);
            self.m_network_error = Requests::NetworkError::UnableToResolveHost;
            self.transition_to_state(State::Error);
        }))
        .when_resolved(weak_callback(*this, [host](auto& self, NonnullRefPtr<DNS::LookupResult const> dns_result) {
            mark_lifecycle_event(&self, &WireStats::dns_completed_at);
            if (dns_result->is_empty() || !dns_result->has_cached_addresses()) {
                dbgln("Request::handle_dns_lookup_state: DNS lookup failed for '{}'", host);
                self.m_network_error = Requests::NetworkError::UnableToResolveHost;
                self.transition_to_state(State::Error);
            } else if (first_is_one_of(self.m_type, RequestType::Fetch, RequestType::BackgroundRevalidation)) {
                self.m_dns_result = move(dns_result);
                self.transition_to_state(State::RetrieveCookie);
            } else if (self.m_type == RequestType::Connect && self.m_connect_cache_level == CacheLevel::CreateConnection) {
                self.m_dns_result = move(dns_result);
                self.transition_to_state(State::Connect);
            } else {
                self.transition_to_state(State::Complete);
            }
        }));
}

void Request::handle_retrieve_cookie_state()
{
    if (m_include_credentials == HTTP::Cookie::IncludeCredentials::No) {
        transition_to_state(State::Fetch);
        return;
    }

    if (auto connection = ConnectionFromClient::primary_connection(); connection.has_value()) {
        mark_lifecycle_event(this, &WireStats::cookie_started_at);
        connection->async_retrieve_http_cookie(m_client.client_id(), m_request_id, m_type, m_url);
    } else {
        m_network_error = Requests::NetworkError::RequestServerDied;
        transition_to_state(State::Error);
    }
}

void Request::handle_connect_state()
{
    m_curl_easy_handle = curl_easy_init();
    if (!m_curl_easy_handle) {
        dbgln("Request::handle_connect_state: Failed to initialize curl easy handle");
        return;
    }

    auto set_option = [&](auto option, auto value) {
        if (auto result = curl_easy_setopt(m_curl_easy_handle, option, value); result != CURLE_OK)
            dbgln("Request::handle_connect_state: Failed to set curl option: {}", curl_easy_strerror(result));
    };

    set_option(CURLOPT_PRIVATE, this);

    set_option(CURLOPT_NOSIGNAL, 1L);

    set_option(CURLOPT_URL, m_url.to_byte_string().characters());
    set_option(CURLOPT_PORT, m_url.port_or_default());
    set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);
    set_option(CURLOPT_CONNECT_ONLY, 1L);

    // Pre-populate the multi's hostcache so libcurl skips its threaded resolver entirely.
    VERIFY(m_dns_result);
    auto formatted_address = build_curl_resolve_list(*m_dns_result, m_url.serialized_host(), m_url.port_or_default());
    if (curl_slist* resolve_list = curl_slist_append(nullptr, formatted_address.characters())) {
        set_option(CURLOPT_RESOLVE, resolve_list);
        m_curl_string_lists.append(resolve_list);
    } else {
        VERIFY_NOT_REACHED();
    }

    mark_lifecycle_event(this, &WireStats::curl_added_at);
    auto result = curl_multi_add_handle(m_curl_multi_handle, m_curl_easy_handle);
    VERIFY(result == CURLM_OK);
}

void Request::handle_fetch_state()
{
    dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: DNS lookup successful");

    m_curl_easy_handle = curl_easy_init();
    if (!m_curl_easy_handle) {
        dbgln("Request::handle_start_fetch_state: Failed to initialize curl easy handle");
        transition_to_state(State::Error);
        return;
    }

    auto is_revalidation_request = this->is_revalidation_request();

    if (!is_revalidation_request) {
        if (inform_client_request_started().is_error())
            return;
    }

    auto set_option = [&](auto option, auto value) {
        if (auto result = curl_easy_setopt(m_curl_easy_handle, option, value); result != CURLE_OK)
            dbgln("Request::handle_start_fetch_state: Failed to set curl option: {}", curl_easy_strerror(result));
    };

    set_option(CURLOPT_PRIVATE, this);

    set_option(CURLOPT_NOSIGNAL, 1L);

    if (auto const& path = default_certificate_path(); !path.is_empty())
        set_option(CURLOPT_CAINFO, path.characters());

    set_option(CURLOPT_ACCEPT_ENCODING, ""); // Empty string lets curl define the accepted encodings.
    set_option(CURLOPT_URL, m_url.to_byte_string().characters());
    set_option(CURLOPT_PORT, m_url.port_or_default());
    set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);
    set_option(CURLOPT_PIPEWAIT, 1L);
    set_option(CURLOPT_ALTSVC, m_alt_svc_cache_path.characters());

    set_option(CURLOPT_CUSTOMREQUEST, m_method.characters());
    set_option(CURLOPT_FOLLOWLOCATION, 0);
    if constexpr (CURL_DEBUG) {
        set_option(CURLOPT_VERBOSE, 1);
    }

#if defined(AK_OS_WINDOWS)
    // Without explicitly using the OS Native CA cert store on Windows, https requests timeout with CURLE_PEER_FAILED_VERIFICATION
    set_option(CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

    curl_slist* curl_headers = nullptr;

    if (m_method.is_one_of("POST"sv, "PUT"sv, "PATCH"sv, "DELETE"sv)) {
        set_option(CURLOPT_POSTFIELDSIZE, m_request_body.size());
        set_option(CURLOPT_POSTFIELDS, m_request_body.data());

        // CURLOPT_POSTFIELDS automatically sets the Content-Type header. Tell curl to remove it by setting a blank
        // value if the headers passed in don't contain a content type.
        if (!m_request_headers->contains("Content-Type"sv))
            curl_headers = curl_slist_append(curl_headers, "Content-Type:");
    } else if (m_method == "HEAD"sv) {
        set_option(CURLOPT_NOBODY, 1L);
    }

    for (auto const& header : *m_request_headers) {
        if (header.value.is_empty()) {
            // curl will discard the header unless we pass the header name followed by a semicolon (i.e. we need to pass
            // "Content-Type;" instead of "Content-Type: ").
            //
            // See: https://curl.se/libcurl/c/httpcustomheader.html
            auto header_string = ByteString::formatted("{};", header.name);
            curl_headers = curl_slist_append(curl_headers, header_string.characters());
        } else {
            auto header_string = ByteString::formatted("{}: {}", header.name, header.value);
            curl_headers = curl_slist_append(curl_headers, header_string.characters());
        }
    }

    if (is_revalidation_request) {
        auto revalidation_attributes = HTTP::RevalidationAttributes::create(m_cache_entry_reader->response_headers());
        VERIFY(revalidation_attributes.etag.has_value() || revalidation_attributes.last_modified.has_value());

        if (revalidation_attributes.etag.has_value()) {
            auto header_string = ByteString::formatted("If-None-Match: {}", *revalidation_attributes.etag);
            curl_headers = curl_slist_append(curl_headers, header_string.characters());
        }

        if (revalidation_attributes.last_modified.has_value()) {
            auto header_string = ByteString::formatted("If-Modified-Since: {}", *revalidation_attributes.last_modified);
            curl_headers = curl_slist_append(curl_headers, header_string.characters());
        }
    }

    if (curl_headers) {
        set_option(CURLOPT_HTTPHEADER, curl_headers);
        m_curl_string_lists.append(curl_headers);
    }

    // FIXME: Set up proxy if applicable
    (void)m_proxy_data;

    set_option(CURLOPT_HEADERFUNCTION, &on_header_received);
    set_option(CURLOPT_HEADERDATA, this);

    set_option(CURLOPT_WRITEFUNCTION, &on_data_received);
    set_option(CURLOPT_WRITEDATA, this);

    if constexpr (REQUESTSERVER_WIRE_DEBUG) {
        set_option(CURLOPT_NOPROGRESS, 0L);
        set_option(CURLOPT_XFERINFOFUNCTION, &on_xferinfo);
        set_option(CURLOPT_XFERINFODATA, this);
    }

    VERIFY(m_dns_result);
    auto formatted_address = build_curl_resolve_list(*m_dns_result, m_url.serialized_host(), m_url.port_or_default());

    if (curl_slist* resolve_list = curl_slist_append(nullptr, formatted_address.characters())) {
        set_option(CURLOPT_RESOLVE, resolve_list);
        m_curl_string_lists.append(resolve_list);
    } else {
        VERIFY_NOT_REACHED();
    }

    mark_lifecycle_event(this, &WireStats::curl_added_at);
    auto result = curl_multi_add_handle(m_curl_multi_handle, m_curl_easy_handle);
    VERIFY(result == CURLM_OK);
}

void Request::handle_complete_state()
{
    if (m_type == RequestType::Fetch) {
        VERIFY(m_curl_result_code.has_value());

        // HTTPS servers might terminate their connection without proper notice of shutdown - i.e. they do not send
        // a "close notify" alert. OpenSSL version 3.2 began treating this as an error, which curl translates to
        // CURLE_RECV_ERROR in the absence of a Content-Length response header. The Python server used by WPT is one
        // such server. We ignore this error if we were actually able to download some response data.
        if (m_curl_result_code == CURLE_RECV_ERROR && m_bytes_transferred_to_client != 0 && !m_response_headers->contains("Content-Length"sv))
            m_curl_result_code = CURLE_OK;

        if (m_curl_result_code != CURLE_OK) {
            m_network_error = curl_code_to_network_error(*m_curl_result_code);

            if (m_network_error == Requests::NetworkError::Unknown) {
                char const* curl_error_message = curl_easy_strerror(static_cast<CURLcode>(*m_curl_result_code));
                dbgln("Request::handle_complete_state: Unable to map error ({}): \"\033[31;1m{}\033[0m\"", *m_curl_result_code, curl_error_message);
            }

            transition_to_state(State::Error);
            return;
        }

        auto timing_info = acquire_timing_info();
        transfer_headers_to_client_if_needed();

        m_client.async_request_finished(m_request_id, m_bytes_transferred_to_client, timing_info, m_network_error);
    }

    m_client.request_complete({}, *this);
}

void Request::handle_error_state()
{
    if (m_type == RequestType::Fetch) {
        // FIXME: Implement timing info for failed requests.
        m_client.async_request_finished(m_request_id, m_bytes_transferred_to_client, {}, m_network_error.value_or(Requests::NetworkError::Unknown));
    }

    m_client.request_complete({}, *this);
}

size_t Request::on_header_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto& request = *static_cast<Request*>(user_data);

    auto total_size = size * nmemb;
    auto header_line = StringView { static_cast<char const*>(buffer), total_size };

    // We need to extract the HTTP reason phrase since it can be a custom value. Fetching infrastructure needs this
    // value for setting the status message.
    if (!request.m_reason_phrase.has_value() && header_line.starts_with("HTTP/"sv)) {
        auto space_index = header_line.find(' ');
        if (space_index.has_value())
            space_index = header_line.find(' ', *space_index + 1);

        if (space_index.has_value()) {
            if (auto reason_phrase = HTTP::normalize_header_value(header_line.substring_view(*space_index + 1)); !reason_phrase.is_empty()) {
                auto decoder = TextCodec::decoder_for_exact_name("ISO-8859-1"sv);
                VERIFY(decoder.has_value());

                request.m_reason_phrase = MUST(decoder->to_utf8(reason_phrase));
                return total_size;
            }
        }
    }

    if (auto colon_index = header_line.find(':'); colon_index.has_value()) {
        auto name = HTTP::normalize_header_value(header_line.substring_view(0, *colon_index));
        auto value = HTTP::normalize_header_value(header_line.substring_view(*colon_index + 1));
        request.m_response_headers->append({ name, value });
    }

    return total_size;
}

size_t Request::on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto& request = *static_cast<Request*>(user_data);

    if (request.m_type == RequestType::Fetch || request.m_type == RequestType::BackgroundRevalidation)
        record_chunk(&request, size * nmemb);

    if (request.is_revalidation_request()) {
        // If we arrive here, we did not receive an HTTP 304 response code. We must remove the cache entry and inform
        // the client of the new response headers and data.
        if (request.revalidation_failed().is_error())
            return CURL_WRITEFUNC_ERROR;

        request.m_disk_cache->create_entry(request, request.m_url, request.m_method, request.m_request_headers, request.m_request_start_time)
            .visit(
                [&](Optional<HTTP::CacheEntryWriter&> cache_entry_writer) {
                    request.m_cache_entry_writer = cache_entry_writer;
                },
                [&](HTTP::DiskCache::CacheHasOpenEntry) {
                    // This should not be reachable, as cache revalidation holds an exclusive lock on the cache entry.
                    VERIFY_NOT_REACHED();
                });
    }

    request.transfer_headers_to_client_if_needed();

    auto total_size = size * nmemb;
    ReadonlyBytes bytes { static_cast<u8 const*>(buffer), total_size };

    auto result = [&] -> ErrorOr<void> {
        TRY(request.m_response_buffer.write_some(bytes));
        return request.write_queued_bytes_without_blocking();
    }();

    if (result.is_error()) {
        dbgln("Request::on_data_received: Aborting request because error occurred whilst writing data to the client: {}", result.error());
        return CURL_WRITEFUNC_ERROR;
    }

    return total_size;
}

ErrorOr<void> Request::inform_client_request_started()
{
    if (m_type == RequestType::BackgroundRevalidation)
        return {};

    auto request_pipe = RequestPipe::create();
    if (request_pipe.is_error()) {
        dbgln("Request::handle_read_from_cache_state: Failed to create pipe: {}", request_pipe.error());
        transition_to_state(State::Error);
        return request_pipe.release_error();
    }

    m_client_request_pipe = request_pipe.release_value();
    m_client.async_request_started(m_request_id, IPC::File::adopt_fd(m_client_request_pipe->reader_fd()));

    return {};
}

void Request::transfer_headers_to_client_if_needed()
{
    if (exchange(m_sent_response_headers_to_client, true))
        return;

    // m_status_code may already be set (e.g. from a resource substitution).
    if (!m_status_code.has_value())
        m_status_code = acquire_status_code();

    if (m_cache_entry_writer.has_value()) {
        if (m_cache_entry_writer->write_status_and_reason(*m_status_code, m_reason_phrase, m_request_headers, m_response_headers).is_error()) {
            m_cache_status = CacheStatus::NotCached;
            m_cache_entry_writer.clear();
        } else {
            m_cache_status = CacheStatus::WrittenToCache;
        }
    }

    if (m_type == RequestType::BackgroundRevalidation)
        return;

    if (m_disk_cache.has_value() && m_disk_cache->mode() == HTTP::DiskCache::Mode::Testing) {
        switch (m_cache_status) {
        case CacheStatus::Unknown:
            break;
        case CacheStatus::NotCached:
            m_response_headers->set({ HTTP::TEST_CACHE_STATUS_HEADER, "not-cached"sv });
            break;
        case CacheStatus::WrittenToCache:
            m_response_headers->set({ HTTP::TEST_CACHE_STATUS_HEADER, "written-to-cache"sv });
            break;
        case CacheStatus::ReadFromCache:
            m_response_headers->set({ HTTP::TEST_CACHE_STATUS_HEADER, "read-from-cache"sv });
            break;
        }
    }

    m_client.async_headers_became_available(m_request_id, m_response_headers->headers(), m_status_code, m_reason_phrase);
}

ErrorOr<void> Request::write_queued_bytes_without_blocking()
{
    auto write_bytes_to_disk_cache = [&](ReadonlyBytes bytes) {
        if (!m_cache_entry_writer.has_value())
            return;

        if (m_cache_entry_writer->write_data(bytes).is_error())
            m_cache_entry_writer.clear();
    };

    if (m_type == RequestType::BackgroundRevalidation) {
        while (!m_response_buffer.is_eof()) {
            auto bytes = m_response_buffer.peek_some_contiguous();
            write_bytes_to_disk_cache(bytes);
            MUST(m_response_buffer.discard(bytes.size()));
        }

        if (m_curl_result_code.has_value())
            transition_to_state(State::Complete);

        return {};
    }

    if (!m_client_writer_notifier) {
        m_client_writer_notifier = Core::Notifier::construct(m_client_request_pipe->writer_fd(), Core::NotificationType::Write);
        m_client_writer_notifier->set_enabled(false);

        m_client_writer_notifier->on_activation = weak_callback(*this, [](auto& self) {
            if (auto result = self.write_queued_bytes_without_blocking(); result.is_error())
                dbgln("Warning: Failed to write buffered request data (it's likely the client disappeared): {}", result.error());
        });
    }

    if constexpr (REQUESTSERVER_WIRE_DEBUG) {
        auto& stats = wire_stats().ensure(this);
        if (stats.current_pressure_window_started.has_value()) {
            auto window = MonotonicTime::now() - *stats.current_pressure_window_started;
            stats.total_pipe_back_pressure = stats.total_pipe_back_pressure + window;
            stats.current_pressure_window_started = {};
            if (window.to_milliseconds() > 50) {
                dbgln("RequestServer wire-pipe-pressure: {} {} unblocked after {} ms (peak buffered={} bytes); WebContent likely behind",
                    m_method, m_url, window.to_milliseconds(), stats.max_buffered_bytes);
            }
        }
    }

    while (!m_response_buffer.is_eof()) {
        auto bytes = m_response_buffer.peek_some_contiguous();

        auto result = m_client_request_pipe->write(bytes);
        if (result.is_error()) {
            if (!first_is_one_of(result.error().code(), EAGAIN, EWOULDBLOCK))
                return result.release_error();

            if constexpr (REQUESTSERVER_WIRE_DEBUG) {
                auto& stats = wire_stats().ensure(this);
                if (!stats.current_pressure_window_started.has_value()) {
                    stats.current_pressure_window_started = MonotonicTime::now();
                    stats.pipe_back_pressure_events += 1;
                    dbgln("RequestServer wire-pipe-pressure: {} {} pipe full, buffering={} bytes",
                        m_method, m_url, m_response_buffer.used_buffer_size());
                }
                if (m_response_buffer.used_buffer_size() > stats.max_buffered_bytes)
                    stats.max_buffered_bytes = m_response_buffer.used_buffer_size();
            }

            m_client_writer_notifier->set_enabled(true);
            return {};
        }

        auto written = result.value();
        write_bytes_to_disk_cache(bytes.slice(0, written));
        MUST(m_response_buffer.discard(written));

        m_bytes_transferred_to_client += written;

        if (written < bytes.size()) {
            m_client_writer_notifier->set_enabled(true);
            return {};
        }
    }

    m_client_writer_notifier->set_enabled(false);
    if (m_curl_result_code.has_value())
        transition_to_state(State::Complete);

    return {};
}

bool Request::is_revalidation_request() const
{
    switch (m_type) {
    case RequestType::Fetch:
        return m_cache_entry_reader.has_value() && m_cache_entry_reader->revalidation_type() == HTTP::CacheEntryReader::RevalidationType::MustRevalidate;
    case RequestType::Connect:
        return false;
    case RequestType::BackgroundRevalidation:
        return m_cache_entry_reader.has_value();
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> Request::revalidation_failed()
{
    if (m_type == RequestType::BackgroundRevalidation && m_disk_cache->mode() == HTTP::DiskCache::Mode::Testing)
        m_response_headers->set({ HTTP::TEST_CACHE_REVALIDATION_STATUS_HEADER, "expired"sv });

    m_cache_entry_reader->revalidation_failed();
    m_cache_entry_reader.clear();

    TRY(inform_client_request_started());
    return {};
}

bool Request::is_cache_only_request() const
{
    if (m_cache_mode == HTTP::CacheMode::OnlyIfCached)
        return true;

    auto cache_control = m_request_headers->get("Cache-Control"sv);
    return cache_control.has_value() && cache_control->contains("only-if-cached"sv, CaseSensitivity::CaseInsensitive);
}

u32 Request::acquire_status_code() const
{
    if (!m_curl_easy_handle)
        return 0;

    long http_status_code = 0;
    auto result = curl_easy_getinfo(m_curl_easy_handle, CURLINFO_RESPONSE_CODE, &http_status_code);
    VERIFY(result == CURLE_OK);

    return static_cast<u32>(http_status_code);
}

Requests::RequestTimingInfo Request::acquire_timing_info() const
{
    // curl_easy_perform()
    // |
    // |--QUEUE
    // |--|--NAMELOOKUP
    // |--|--|--CONNECT
    // |--|--|--|--APPCONNECT
    // |--|--|--|--|--PRETRANSFER
    // |--|--|--|--|--|--POSTTRANSFER
    // |--|--|--|--|--|--|--STARTTRANSFER
    // |--|--|--|--|--|--|--|--TOTAL
    // |--|--|--|--|--|--|--|--REDIRECT

    // FIXME: Implement timing info for cache hits.
    if (m_cache_entry_reader.has_value())
        return {};

    // No timing info available for resource substitutions (no curl handle).
    if (!m_curl_easy_handle)
        return {};

    auto get_timing_info = [&](auto option) {
        curl_off_t time_value = 0;
        auto result = curl_easy_getinfo(m_curl_easy_handle, option, &time_value);
        VERIFY(result == CURLE_OK);
        return time_value;
    };

    auto queue_time = get_timing_info(CURLINFO_QUEUE_TIME_T);
    auto domain_lookup_time = get_timing_info(CURLINFO_NAMELOOKUP_TIME_T);
    auto connect_time = get_timing_info(CURLINFO_CONNECT_TIME_T);
    auto secure_connect_time = get_timing_info(CURLINFO_APPCONNECT_TIME_T);
    auto request_start_time = get_timing_info(CURLINFO_PRETRANSFER_TIME_T);
    auto response_start_time = get_timing_info(CURLINFO_STARTTRANSFER_TIME_T);
    auto response_end_time = get_timing_info(CURLINFO_TOTAL_TIME_T);
    auto encoded_body_size = get_timing_info(CURLINFO_SIZE_DOWNLOAD_T);

    long http_version = 0;
    auto get_version_result = curl_easy_getinfo(m_curl_easy_handle, CURLINFO_HTTP_VERSION, &http_version);
    VERIFY(get_version_result == CURLE_OK);

    auto http_version_alpn = Requests::ALPNHttpVersion::None;
    switch (http_version) {
    case CURL_HTTP_VERSION_1_0:
        http_version_alpn = Requests::ALPNHttpVersion::Http1_0;
        break;
    case CURL_HTTP_VERSION_1_1:
        http_version_alpn = Requests::ALPNHttpVersion::Http1_1;
        break;
    case CURL_HTTP_VERSION_2_0:
        http_version_alpn = Requests::ALPNHttpVersion::Http2_TLS;
        break;
    case CURL_HTTP_VERSION_3:
        http_version_alpn = Requests::ALPNHttpVersion::Http3;
        break;
    default:
        http_version_alpn = Requests::ALPNHttpVersion::None;
        break;
    }

    return Requests::RequestTimingInfo {
        .domain_lookup_start_microseconds = queue_time,
        .domain_lookup_end_microseconds = queue_time + domain_lookup_time,
        .connect_start_microseconds = queue_time + domain_lookup_time,
        .connect_end_microseconds = queue_time + domain_lookup_time + connect_time + secure_connect_time,
        .secure_connect_start_microseconds = queue_time + domain_lookup_time + connect_time,
        .request_start_microseconds = queue_time + domain_lookup_time + connect_time + secure_connect_time + request_start_time,
        .response_start_microseconds = queue_time + domain_lookup_time + connect_time + secure_connect_time + response_start_time,
        .response_end_microseconds = queue_time + domain_lookup_time + connect_time + secure_connect_time + response_end_time,
        .encoded_body_size = encoded_body_size,
        .http_version_alpn_identifier = http_version_alpn,
    };
}

}
