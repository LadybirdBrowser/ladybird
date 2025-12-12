/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibCore/Notifier.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibTextCodec/Decoder.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Request.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static long s_connect_timeout_seconds = 90L;

NonnullOwnPtr<Request> Request::fetch(
    i32 request_id,
    Optional<HTTP::DiskCache&> disk_cache,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    NonnullRefPtr<HTTP::HeaderList> request_headers,
    ByteBuffer request_body,
    ByteString alt_svc_cache_path,
    Core::ProxyData proxy_data)
{
    auto request = adopt_own(*new Request { request_id, disk_cache, client, curl_multi, resolver, move(url), move(method), move(request_headers), move(request_body), move(alt_svc_cache_path), proxy_data });
    request->process();

    return request;
}

NonnullOwnPtr<Request> Request::connect(
    i32 request_id,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    CacheLevel cache_level)
{
    auto request = adopt_own(*new Request { request_id, client, curl_multi, resolver, move(url) });

    switch (cache_level) {
    case CacheLevel::ResolveOnly:
        request->transition_to_state(State::DNSLookup);
        break;
    case CacheLevel::CreateConnection:
        request->transition_to_state(State::Connect);
        break;
    }

    return request;
}

Request::Request(
    i32 request_id,
    Optional<HTTP::DiskCache&> disk_cache,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    NonnullRefPtr<HTTP::HeaderList> request_headers,
    ByteBuffer request_body,
    ByteString alt_svc_cache_path,
    Core::ProxyData proxy_data)
    : m_request_id(request_id)
    , m_type(Type::Fetch)
    , m_disk_cache(disk_cache)
    , m_client(client)
    , m_curl_multi_handle(curl_multi)
    , m_resolver(resolver)
    , m_url(move(url))
    , m_method(move(method))
    , m_request_headers(move(request_headers))
    , m_request_body(move(request_body))
    , m_alt_svc_cache_path(move(alt_svc_cache_path))
    , m_proxy_data(proxy_data)
    , m_response_headers(HTTP::HeaderList::create())
{
}

Request::Request(
    i32 request_id,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url)
    : m_request_id(request_id)
    , m_type(Type::Connect)
    , m_client(client)
    , m_curl_multi_handle(curl_multi)
    , m_resolver(resolver)
    , m_url(move(url))
    , m_request_headers(HTTP::HeaderList::create())
    , m_response_headers(HTTP::HeaderList::create())
{
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

    if (m_cache_entry_writer.has_value())
        (void)m_cache_entry_writer->flush(m_response_headers);
}

void Request::notify_request_unblocked(Badge<HTTP::DiskCache>)
{
    // FIXME: We may want a timer to limit how long we are waiting for a request before proceeding with a network
    //        request that skips the disk cache.
    transition_to_state(State::Init);
}

void Request::notify_fetch_complete(Badge<ConnectionFromClient>, int result_code)
{
    if (m_cache_entry_reader.has_value() && m_cache_entry_reader->must_revalidate()) {
        if (acquire_status_code() == 304) {
            m_cache_entry_reader->revalidation_succeeded(m_response_headers);
            transition_to_state(State::ReadCache);
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
    case State::DNSLookup:
        handle_dns_lookup_state();
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
    if (m_disk_cache.has_value()) {
        m_disk_cache->open_entry(*this, m_url, m_method, m_request_headers)
            .visit(
                [&](Optional<HTTP::CacheEntryReader&> cache_entry_reader) {
                    m_cache_entry_reader = cache_entry_reader;

                    if (m_cache_entry_reader.has_value()) {
                        if (m_cache_entry_reader->must_revalidate())
                            transition_to_state(State::DNSLookup);
                        else
                            transition_to_state(State::ReadCache);
                    }
                },
                [&](HTTP::DiskCache::CacheHasOpenEntry) {
                    // If an existing entry is open for writing, we must wait for it to complete.
                    transition_to_state(State::WaitForCache);
                });

        if (m_state != State::Init)
            return;

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
    m_reason_phrase = m_cache_entry_reader->reason_phrase();
    m_response_headers = m_cache_entry_reader->response_headers();
    m_cache_status = CacheStatus::ReadFromCache;

    if (inform_client_request_started().is_error())
        return;
    transfer_headers_to_client_if_needed();

    m_cache_entry_reader->pipe_to(
        m_client_request_pipe->writer_fd(),
        [this](auto bytes_sent) {
            m_bytes_transferred_to_client = bytes_sent;
            m_curl_result_code = CURLE_OK;

            transition_to_state(State::Complete);
        },
        [this](auto bytes_sent) {
            m_bytes_transferred_to_client = bytes_sent;
            m_network_error = Requests::NetworkError::CacheReadFailed;

            transition_to_state(State::Error);
        });
}

void Request::handle_dns_lookup_state()
{
    auto host = m_url.serialized_host().to_byte_string();
    auto const& dns_info = DNSInfo::the();

    m_resolver->dns.lookup(host, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA }, { .validate_dnssec_locally = dns_info.validate_dnssec_locally })
        ->when_rejected([this, host](auto const& error) {
            dbgln("Request::handle_dns_lookup_state: DNS lookup failed for '{}': {}", host, error);
            m_network_error = Requests::NetworkError::UnableToResolveHost;
            transition_to_state(State::Error);
        })
        .when_resolved([this, host](NonnullRefPtr<DNS::LookupResult const> dns_result) mutable {
            if (dns_result->is_empty() || !dns_result->has_cached_addresses()) {
                dbgln("Request::handle_dns_lookup_state: DNS lookup failed for '{}'", host);
                m_network_error = Requests::NetworkError::UnableToResolveHost;
                transition_to_state(State::Error);
            } else if (m_type == Type::Fetch) {
                m_dns_result = move(dns_result);
                transition_to_state(State::Fetch);
            } else {
                transition_to_state(State::Complete);
            }
        });
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
    set_option(CURLOPT_URL, m_url.to_byte_string().characters());
    set_option(CURLOPT_PORT, m_url.port_or_default());
    set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);
    set_option(CURLOPT_CONNECT_ONLY, 1L);

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

    auto is_revalidation_request = m_cache_entry_reader.has_value() && m_cache_entry_reader->must_revalidate();

    if (!is_revalidation_request) {
        if (inform_client_request_started().is_error())
            return;
    }

    auto set_option = [&](auto option, auto value) {
        if (auto result = curl_easy_setopt(m_curl_easy_handle, option, value); result != CURLE_OK)
            dbgln("Request::handle_start_fetch_state: Failed to set curl option: {}", curl_easy_strerror(result));
    };

    set_option(CURLOPT_PRIVATE, this);

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
    set_option(CURLOPT_LADYBIRD_QUIRKS_MODE, 1); // Deal with networking web compatibility issues. See the comment on the option.
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
            // There is no CURLOPT for If-None-Match, so we must set the header value directly.
            auto header_string = ByteString::formatted("If-None-Match: {}", *revalidation_attributes.etag);
            curl_headers = curl_slist_append(curl_headers, header_string.characters());
        }

        if (revalidation_attributes.last_modified.has_value()) {
            set_option(CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
            set_option(CURLOPT_TIMEVALUE, revalidation_attributes.last_modified->seconds_since_epoch());
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

    VERIFY(m_dns_result);
    auto formatted_address = build_curl_resolve_list(*m_dns_result, m_url.serialized_host(), m_url.port_or_default());

    if (curl_slist* resolve_list = curl_slist_append(nullptr, formatted_address.characters())) {
        set_option(CURLOPT_RESOLVE, resolve_list);
        m_curl_string_lists.append(resolve_list);
    } else {
        VERIFY_NOT_REACHED();
    }

    auto result = curl_multi_add_handle(m_curl_multi_handle, m_curl_easy_handle);
    VERIFY(result == CURLM_OK);
}

void Request::handle_complete_state()
{
    if (m_type == Type::Fetch) {
        VERIFY(m_curl_result_code.has_value());

        auto timing_info = acquire_timing_info();
        transfer_headers_to_client_if_needed();

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
        }

        m_client.async_request_finished(m_request_id, m_bytes_transferred_to_client, timing_info, m_network_error);
    }

    m_client.request_complete({}, m_request_id);
}

void Request::handle_error_state()
{
    if (m_type == Type::Fetch) {
        // FIXME: Implement timing info for failed requests.
        m_client.async_request_finished(m_request_id, m_bytes_transferred_to_client, {}, m_network_error.value_or(Requests::NetworkError::Unknown));
    }

    m_client.request_complete({}, m_request_id);
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
            if (auto reason_phrase = header_line.substring_view(*space_index + 1).trim_whitespace(); !reason_phrase.is_empty()) {
                auto decoder = TextCodec::decoder_for_exact_name("ISO-8859-1"sv);
                VERIFY(decoder.has_value());

                request.m_reason_phrase = MUST(decoder->to_utf8(reason_phrase));
                return total_size;
            }
        }
    }

    if (auto colon_index = header_line.find(':'); colon_index.has_value()) {
        auto name = header_line.substring_view(0, *colon_index).trim_whitespace();
        auto value = header_line.substring_view(*colon_index + 1).trim_whitespace();
        request.m_response_headers->append({ name, value });
    }

    return total_size;
}

size_t Request::on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto& request = *static_cast<Request*>(user_data);

    if (request.m_cache_entry_reader.has_value() && request.m_cache_entry_reader->must_revalidate()) {
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

    if (m_cache_entry_reader.has_value())
        m_status_code = m_cache_entry_reader->status_code();
    else
        m_status_code = acquire_status_code();

    if (m_cache_entry_writer.has_value()) {
        if (m_cache_entry_writer->write_status_and_reason(m_status_code, m_reason_phrase, m_response_headers).is_error()) {
            m_cache_status = CacheStatus::NotCached;
            m_cache_entry_writer.clear();
        } else {
            m_cache_status = CacheStatus::WrittenToCache;
        }
    }

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
    if (!m_client_writer_notifier) {
        m_client_writer_notifier = Core::Notifier::construct(m_client_request_pipe->writer_fd(), Core::NotificationType::Write);
        m_client_writer_notifier->set_enabled(false);

        m_client_writer_notifier->on_activation = [this] {
            if (auto result = write_queued_bytes_without_blocking(); result.is_error())
                dbgln("Warning: Failed to write buffered request data (it's likely the client disappeared): {}", result.error());
        };
    }

    Vector<u8> bytes_to_send;
    bytes_to_send.resize(m_response_buffer.used_buffer_size());
    m_response_buffer.peek_some(bytes_to_send);

    auto result = m_client_request_pipe->write(bytes_to_send);
    if (result.is_error()) {
        if (!first_is_one_of(result.error().code(), EAGAIN, EWOULDBLOCK))
            return result.release_error();

        m_client_writer_notifier->set_enabled(true);
        return {};
    }

    if (m_cache_entry_writer.has_value()) {
        auto bytes_sent = bytes_to_send.span().slice(0, result.value());

        if (m_cache_entry_writer->write_data(bytes_sent).is_error())
            m_cache_entry_writer.clear();
    }

    m_bytes_transferred_to_client += result.value();
    MUST(m_response_buffer.discard(result.value()));

    m_client_writer_notifier->set_enabled(!m_response_buffer.is_eof());
    if (m_response_buffer.is_eof() && m_curl_result_code.has_value())
        transition_to_state(State::Complete);

    return {};
}

ErrorOr<void> Request::revalidation_failed()
{
    m_cache_entry_reader->revalidation_failed();
    m_cache_entry_reader.clear();

    TRY(inform_client_request_started());
    return {};
}

u32 Request::acquire_status_code() const
{
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
