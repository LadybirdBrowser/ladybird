/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibTextCodec/Decoder.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Cache/DiskCache.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Quarantine.h>
#include <RequestServer/Request.h>
#include <RequestServer/Resolver.h>

namespace RequestServer {

static long s_connect_timeout_seconds = 90L;

NonnullOwnPtr<Request> Request::fetch(
    i32 request_id,
    Optional<DiskCache&> disk_cache,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    HTTP::HeaderMap request_headers,
    ByteBuffer request_body,
    ByteString alt_svc_cache_path,
    Core::ProxyData proxy_data,
    RefPtr<IPC::NetworkIdentity> network_identity)
{
    auto request = adopt_own(*new Request { request_id, disk_cache, client, curl_multi, resolver, move(url), move(method), move(request_headers), move(request_body), move(alt_svc_cache_path), proxy_data });
    request->m_network_identity = move(network_identity);
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
    Optional<DiskCache&> disk_cache,
    ConnectionFromClient& client,
    void* curl_multi,
    Resolver& resolver,
    URL::URL url,
    ByteString method,
    HTTP::HeaderMap request_headers,
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
        (void)m_cache_entry_writer->flush();
}

void Request::notify_request_unblocked(Badge<DiskCache>)
{
    // FIXME: We may want a timer to limit how long we are waiting for a request before proceeding with a network
    //        request that skips the disk cache.
    transition_to_state(State::Init);
}

void Request::notify_fetch_complete(Badge<ConnectionFromClient>, int result_code)
{
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
    case State::WaitingForPolicy:
        handle_waiting_for_policy_state();
        break;
    case State::PolicyBlocked:
    case State::PolicyQuarantined:
        // These states are terminal - they transition to Complete or Error
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
        m_disk_cache->open_entry(*this).visit(
            [&](Optional<CacheEntryReader&> cache_entry_reader) {
                m_cache_entry_reader = cache_entry_reader;
                if (m_cache_entry_reader.has_value())
                    transition_to_state(State::ReadCache);
            },
            [&](DiskCache::CacheHasOpenEntry) {
                // If an existing entry is open for writing, we must wait for it to complete.
                transition_to_state(State::WaitForCache);
            });

        if (m_state != State::Init)
            return;

        m_disk_cache->create_entry(*this).visit(
            [&](Optional<CacheEntryWriter&> cache_entry_writer) {
                m_cache_entry_writer = cache_entry_writer;
            },
            [&](DiskCache::CacheHasOpenEntry) {
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
#if defined(AK_OS_WINDOWS)
    dbgln("FIXME: Request::handle_read_from_cache_state: Not implemented on Windows");
    transition_to_state(State::Error);
#else
    m_status_code = m_cache_entry_reader->status_code();
    m_reason_phrase = m_cache_entry_reader->reason_phrase();
    m_response_headers = m_cache_entry_reader->headers();

    auto pipe_or_error = RequestPipe::create();
    if (pipe_or_error.is_error()) {
        dbgln("Request::handle_read_from_cache_state: Failed to create pipe: {}", pipe_or_error.error());
        transition_to_state(State::Error);
        return;
    }

    auto pipe = pipe_or_error.release_value();

    m_client.async_request_started(m_request_id, IPC::File::adopt_fd(pipe.reader_fd()));
    m_client_request_pipe = move(pipe);

    m_client.async_headers_became_available(m_request_id, m_response_headers, m_status_code, m_reason_phrase);
    m_sent_response_headers_to_client = true;

    m_cache_entry_reader->pipe_to(
        m_client_request_pipe.value().writer_fd(),
        [this](auto bytes_sent) {
            m_bytes_transferred_to_client = bytes_sent;
            m_curl_result_code = CURLE_OK;

            transition_to_state(State::Complete);
        },
        [this](auto bytes_sent) {
            // FIXME: We should also have a way to validate the data once CacheEntry is storing its crc.
            m_start_offset_of_response_resumed_from_cache = bytes_sent;
            m_disk_cache.clear();

            transition_to_state(State::DNSLookup);
        });
#endif
}

void Request::handle_dns_lookup_state()
{
    // Skip DNS lookup for SOCKS5H proxy (Tor) - let proxy handle DNS resolution
    if (m_network_identity && m_network_identity->has_proxy()) {
        auto const& proxy_config = m_network_identity->proxy_config();
        if (proxy_config.has_value() && proxy_config->type == IPC::ProxyType::SOCKS5H) {
            auto host = m_url.serialized_host().to_byte_string();
            dbgln("RequestServer: Skipping DNS lookup for '{}' (using SOCKS5H proxy - DNS via Tor)", host);
            // Skip DNS, transition directly to fetch state
            transition_to_state(State::Fetch);
            return;
        }
    }

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

    if (!m_start_offset_of_response_resumed_from_cache.has_value()) {
        auto pipe_or_error = RequestPipe::create();
        if (pipe_or_error.is_error()) {
            dbgln("Request::handle_start_fetch_state: Failed to create pipe: {}", pipe_or_error.error());
            transition_to_state(State::Error);
            return;
        }

        auto pipe = pipe_or_error.release_value();

        m_client.async_request_started(m_request_id, IPC::File::adopt_fd(pipe.reader_fd()));
        m_client_request_pipe = move(pipe);
    }

    m_client_writer_notifier = Core::Notifier::construct(m_client_request_pipe.value().writer_fd(), Core::NotificationType::Write);
    m_client_writer_notifier->set_enabled(false);

    m_client_writer_notifier->on_activation = [this] {
        if (auto result = write_queued_bytes_without_blocking(); result.is_error())
            dbgln("Warning: Failed to write buffered request data (it's likely the client disappeared): {}", result.error());
    };

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

    curl_slist* curl_headers = nullptr;

    if (m_method.is_one_of("POST"sv, "PUT"sv, "PATCH"sv, "DELETE"sv)) {
        set_option(CURLOPT_POSTFIELDSIZE, m_request_body.size());
        set_option(CURLOPT_POSTFIELDS, m_request_body.data());

        // CURLOPT_POSTFIELDS automatically sets the Content-Type header. Tell curl to remove it by setting a blank
        // value if the headers passed in don't contain a content type.
        if (!m_request_headers.contains("Content-Type"sv))
            curl_headers = curl_slist_append(curl_headers, "Content-Type:");
    } else if (m_method == "HEAD"sv) {
        set_option(CURLOPT_NOBODY, 1L);
    }

    for (auto const& header : m_request_headers.headers()) {
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

    if (curl_headers) {
        set_option(CURLOPT_HTTPHEADER, curl_headers);
        m_curl_string_lists.append(curl_headers);
    }

    if (m_start_offset_of_response_resumed_from_cache.has_value()) {
        auto range = ByteString::formatted("{}-", *m_start_offset_of_response_resumed_from_cache);
        set_option(CURLOPT_RANGE, range.characters());
    }

    // Apply proxy configuration from NetworkIdentity (Tor/VPN support)
    if (m_network_identity && m_network_identity->has_proxy()) {
        auto const& proxy_config = m_network_identity->proxy_config();
        if (proxy_config.has_value()) {
            // Set proxy URL (e.g., "socks5h://localhost:9050" for Tor)
            auto proxy_url = proxy_config->to_curl_proxy_url();
            set_option(CURLOPT_PROXY, proxy_url.characters());

            // Set proxy type for libcurl
            switch (proxy_config->type) {
            case IPC::ProxyType::SOCKS5H:
                set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);  // DNS via proxy
                dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Using SOCKS5H proxy at {} (DNS via proxy)", proxy_url);
                break;
            case IPC::ProxyType::SOCKS5:
                set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);  // Local DNS
                dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Using SOCKS5 proxy at {}", proxy_url);
                break;
            case IPC::ProxyType::HTTP:
                set_option(CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
                dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Using HTTP proxy at {}", proxy_url);
                break;
            case IPC::ProxyType::HTTPS:
                set_option(CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);
                dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Using HTTPS proxy at {}", proxy_url);
                break;
            case IPC::ProxyType::None:
                break;
            }

            // Set SOCKS5 authentication for stream isolation (Tor circuit isolation)
            if (auto auth = proxy_config->to_curl_auth_string(); auth.has_value()) {
                set_option(CURLOPT_PROXYUSERPWD, auth->characters());
                dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Using proxy authentication for circuit isolation");
            }
        }
    }

    set_option(CURLOPT_HEADERFUNCTION, &on_header_received);
    set_option(CURLOPT_HEADERDATA, this);

    set_option(CURLOPT_WRITEFUNCTION, &on_data_received);
    set_option(CURLOPT_WRITEDATA, this);

    // Only apply DNS resolution if we have a DNS result
    // For SOCKS5H proxy, m_dns_result will be null and proxy handles DNS
    if (m_dns_result) {
        auto formatted_address = build_curl_resolve_list(*m_dns_result, m_url.serialized_host(), m_url.port_or_default());

        if (curl_slist* resolve_list = curl_slist_append(nullptr, formatted_address.characters())) {
            set_option(CURLOPT_RESOLVE, resolve_list);
            m_curl_string_lists.append(resolve_list);
        } else {
            VERIFY_NOT_REACHED();
        }
    } else {
        dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Skipping CURLOPT_RESOLVE (DNS resolution via proxy)");
    }

    auto result = curl_multi_add_handle(m_curl_multi_handle, m_curl_easy_handle);
    VERIFY(result == CURLM_OK);

    // Log request for audit trail
    if (m_network_identity) {
        m_network_identity->log_request(m_url, m_method);
    }
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
        if (m_curl_result_code == CURLE_RECV_ERROR && m_bytes_transferred_to_client != 0 && !m_response_headers.contains("Content-Length"sv))
            m_curl_result_code = CURLE_OK;

        if (m_curl_result_code != CURLE_OK) {
            m_network_error = curl_code_to_network_error(*m_curl_result_code);

            if (m_network_error == Requests::NetworkError::Unknown) {
                char const* curl_error_message = curl_easy_strerror(static_cast<CURLcode>(*m_curl_result_code));
                dbgln("Request::handle_complete_state: Unable to map error ({}): \"\033[31;1m{}\033[0m\"", *m_curl_result_code, curl_error_message);
            }
        }

        // IPFS Integration: Content verification hook
        // Note: Verification happens on data already sent to client. If verification fails,
        // the error will be reported but data has already been transferred.
        if (m_content_verification_callback && !m_network_error.has_value() && m_bytes_transferred_to_client > 0) {
            // Allocate buffer for post-transfer verification check
            // FIXME: For large files, this could be memory-intensive. Consider streaming verification.
            auto buffer_size = m_response_buffer.used_buffer_size();
            if (buffer_size > 0) {
                auto verification_buffer_result = ByteBuffer::create_uninitialized(buffer_size);
                if (verification_buffer_result.is_error()) {
                    dbgln("Request::handle_complete_state: Failed to allocate buffer for content verification");
                    m_network_error = Requests::NetworkError::Unknown;
                } else {
                    auto& verification_buffer = verification_buffer_result.value();
                    auto read_result = m_response_buffer.read_until_filled(verification_buffer);

                    if (read_result.is_error()) {
                        dbgln("Request::handle_complete_state: Failed to read response buffer for verification: {}", read_result.error());
                        m_network_error = Requests::NetworkError::Unknown;
                    } else {
                        auto verification_result = m_content_verification_callback(verification_buffer.bytes());
                        if (verification_result.is_error()) {
                            dbgln("Request::handle_complete_state: Content verification failed: {}", verification_result.error());
                            m_network_error = Requests::NetworkError::Unknown;
                        } else if (!verification_result.value()) {
                            dbgln("Request::handle_complete_state: Content integrity check failed");
                            m_network_error = Requests::NetworkError::Unknown;
                        }
                    }
                }
            }
        }

        m_client.async_request_finished(m_request_id, m_bytes_transferred_to_client, timing_info, m_network_error);

        // Log response for audit trail
        if (m_network_identity && !m_network_error.has_value()) {
            auto status_code = acquire_status_code();
            // Calculate total bytes for this request
            size_t bytes_sent = m_request_body.size();
            size_t bytes_received = m_bytes_transferred_to_client;

            m_network_identity->log_response(m_url, static_cast<u16>(status_code), bytes_sent, bytes_received);
        }

        // Sentinel SecurityTap integration - inspect downloads for threats
        if (m_security_tap && should_inspect_download() && !m_network_error.has_value()) {
            auto buffer_size = m_response_buffer.used_buffer_size();
            if (buffer_size > 0) {
                // Read content from response buffer
                auto content_buffer_result = ByteBuffer::create_uninitialized(buffer_size);
                if (!content_buffer_result.is_error()) {
                    auto content_buffer = content_buffer_result.release_value();
                    auto read_result = m_response_buffer.read_until_filled(content_buffer);

                    if (!read_result.is_error()) {
                        // Extract download metadata
                        auto metadata = extract_download_metadata();

                        // Compute SHA256 hash
                        auto sha256_result = SecurityTap::compute_sha256(content_buffer.bytes());
                        if (!sha256_result.is_error()) {
                            const_cast<SecurityTap::DownloadMetadata&>(metadata).sha256 = sha256_result.release_value();

                            // Scan the content
                            auto scan_result = m_security_tap->inspect_download(metadata, content_buffer.bytes());

                            if (!scan_result.is_error() && scan_result.value().is_threat) {
                                dbgln("SecurityTap: Threat detected in download: {}", metadata.filename);
                                // Store alert JSON for quarantine (Phase 3 Day 19)
                                m_security_alert_json = scan_result.value().alert_json.value();
                                // Send security alert to browser via IPC (with page_id for routing)
                                m_client.async_security_alert(m_request_id, m_page_id, scan_result.value().alert_json.value());
                            } else if (scan_result.is_error()) {
                                dbgln("SecurityTap: Scan failed: {}", scan_result.error());
                            }
                        }

                        // Note: AllocatingMemoryStream is already at the correct position
                        // No need to rewind - the read_until_filled() doesn't move the position
                    }
                }
            }
        }
    }

    m_client.request_complete({}, m_request_id);
}

void Request::handle_error_state()
{
    // IPFS Integration: Try gateway fallback if available for recoverable errors
    if (m_gateway_fallback_callback) {
        auto error = m_network_error.value_or(Requests::NetworkError::Unknown);
        // Retry on DNS, connection, timeout, or unknown errors (typical gateway failures)
        if (error == Requests::NetworkError::UnableToResolveHost
            || error == Requests::NetworkError::UnableToConnect
            || error == Requests::NetworkError::TimeoutReached
            || error == Requests::NetworkError::Unknown) {
            dbgln("Request::handle_error_state: Triggering gateway fallback for error: {}", static_cast<int>(error));
            m_gateway_fallback_callback();
            // Don't send async_request_finished - fallback will create new request
            m_client.request_complete({}, m_request_id);
            return;
        }
    }

    if (m_type == Type::Fetch) {
        // FIXME: Implement timing info for failed requests.
        m_client.async_request_finished(m_request_id, m_bytes_transferred_to_client, {}, m_network_error.value_or(Requests::NetworkError::Unknown));
    }

    m_client.request_complete({}, m_request_id);
}

bool Request::should_inspect_download() const
{
    // Only inspect actual downloads, not page navigations or API responses

    // Check Content-Disposition header
    auto content_disposition = m_response_headers.get("Content-Disposition"sv);
    if (content_disposition.has_value() && content_disposition->contains("attachment"sv))
        return true;

    // Check for common download MIME types
    auto content_type = m_response_headers.get("Content-Type"sv);
    if (content_type.has_value()) {
        // Applications (executables, archives, documents)
        if (content_type->starts_with("application/"sv))
            return true;
        // Executables
        if (content_type->contains("executable"sv) || content_type->contains("x-ms"sv))
            return true;
    }

    // Check URL file extension for common download types
    auto path = m_url.serialize_path().to_byte_string();
    if (path.ends_with(".exe"sv) || path.ends_with(".msi"sv) || path.ends_with(".dmg"sv)
        || path.ends_with(".zip"sv) || path.ends_with(".rar"sv) || path.ends_with(".7z"sv)
        || path.ends_with(".tar"sv) || path.ends_with(".gz"sv)
        || path.ends_with(".ps1"sv) || path.ends_with(".bat"sv) || path.ends_with(".sh"sv)
        || path.ends_with(".apk"sv) || path.ends_with(".deb"sv) || path.ends_with(".rpm"sv))
        return true;

    return false;
}

SecurityTap::DownloadMetadata Request::extract_download_metadata() const
{
    // Extract filename from Content-Disposition header or URL
    ByteString filename = "unknown"sv;

    auto disposition = m_response_headers.get("Content-Disposition"sv);
    if (disposition.has_value()) {
        // Parse: Content-Disposition: attachment; filename="file.exe"
        auto filename_pos = disposition->find("filename="sv);
        if (filename_pos.has_value()) {
            auto start = *filename_pos + 9; // length of "filename="
            auto filename_part = disposition->substring_view(start);

            // Remove quotes if present
            if (filename_part.starts_with('"')) {
                filename_part = filename_part.substring_view(1);
                if (auto quote_end = filename_part.find('"'); quote_end.has_value())
                    filename_part = filename_part.substring_view(0, *quote_end);
            } else {
                // Without quotes, filename ends at semicolon or end of string
                if (auto semicolon = filename_part.find(';'); semicolon.has_value())
                    filename_part = filename_part.substring_view(0, *semicolon);
            }

            filename = filename_part.trim_whitespace();
        }
    }

    // Fallback: extract from URL path
    if (filename == "unknown"sv) {
        auto path = m_url.serialize_path().to_byte_string();
        if (auto last_slash = path.find_last('/'); last_slash.has_value()) {
            filename = path.substring_view(*last_slash + 1);
        } else {
            filename = path;
        }

        // If still empty, use a generic name
        if (filename.is_empty())
            filename = "download"sv;
    }

    auto mime_type = m_response_headers.get("Content-Type"sv).value_or("application/octet-stream"sv);

    return SecurityTap::DownloadMetadata {
        .url = m_url.to_byte_string(),
        .filename = filename,
        .mime_type = mime_type,
        .sha256 = ""sv, // Computed by SecurityTap
        .size_bytes = m_response_buffer.used_buffer_size()
    };
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
        request.m_response_headers.set(name, value);
    }

    return total_size;
}

size_t Request::on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto& request = *static_cast<Request*>(user_data);
    request.transfer_headers_to_client_if_needed();

    auto total_size = size * nmemb;
    ReadonlyBytes bytes { static_cast<u8 const*>(buffer), total_size };

    // Sentinel integration: Incremental scanning for malware detection
    if (request.m_security_tap && request.should_inspect_download()) {
        // Write to response buffer first (needed for scanning)
        auto write_result = request.m_response_buffer.write_some(bytes);
        if (write_result.is_error()) {
            dbgln("Request::on_data_received: Failed to write to response buffer: {}", write_result.error());
            return CURL_WRITEFUNC_ERROR;
        }

        // Scan the accumulated content incrementally
        auto buffer_size = request.m_response_buffer.used_buffer_size();
        if (buffer_size > 0) {
            auto content_buffer_result = ByteBuffer::create_uninitialized(buffer_size);
            if (!content_buffer_result.is_error()) {
                auto content_buffer = content_buffer_result.release_value();

                // Peek at the data (don't consume it)
                request.m_response_buffer.peek_some(content_buffer.span());

                // Extract download metadata
                auto metadata = request.extract_download_metadata();

                // Compute SHA256 hash
                auto sha256_result = SecurityTap::compute_sha256(content_buffer.bytes());
                if (!sha256_result.is_error()) {
                    const_cast<SecurityTap::DownloadMetadata&>(metadata).sha256 = sha256_result.release_value();

                    // Scan the content
                    auto scan_result = request.m_security_tap->inspect_download(metadata, content_buffer.bytes());

                    if (!scan_result.is_error() && scan_result.value().is_threat) {
                        dbgln("SecurityTap: Threat detected during download: {}", metadata.filename);

                        // Store alert JSON for quarantine (Phase 3 Day 19)
                        request.m_security_alert_json = scan_result.value().alert_json.value();

                        // Send security alert to browser via IPC
                        request.m_client.async_security_alert(request.m_request_id, request.m_page_id, scan_result.value().alert_json.value());

                        // Transition to WaitingForPolicy state
                        request.transition_to_state(State::WaitingForPolicy);

                        // Pause CURL transfer
                        return CURL_WRITEFUNC_PAUSE;
                    }
                }
            }
        }

        // Continue normal processing
        auto flush_result = request.write_queued_bytes_without_blocking();
        if (flush_result.is_error()) {
            dbgln("Request::on_data_received: Aborting request because error occurred whilst writing data to the client: {}", flush_result.error());
            return CURL_WRITEFUNC_ERROR;
        }
    } else {
        // Normal path (no security scanning)
        auto result = [&] -> ErrorOr<void> {
            TRY(request.m_response_buffer.write_some(bytes));
            return request.write_queued_bytes_without_blocking();
        }();

        if (result.is_error()) {
            dbgln("Request::on_data_received: Aborting request because error occurred whilst writing data to the client: {}", result.error());
            return CURL_WRITEFUNC_ERROR;
        }
    }

    return total_size;
}

void Request::transfer_headers_to_client_if_needed()
{
    if (exchange(m_sent_response_headers_to_client, true))
        return;

    m_status_code = acquire_status_code();
    m_client.async_headers_became_available(m_request_id, m_response_headers, m_status_code, m_reason_phrase);

    if (m_cache_entry_writer.has_value()) {
        if (m_cache_entry_writer->write_headers(m_status_code, m_reason_phrase, m_response_headers).is_error())
            m_cache_entry_writer.clear();
    }
}

ErrorOr<void> Request::write_queued_bytes_without_blocking()
{
    auto available_bytes = m_response_buffer.used_buffer_size();

    // If we've received a response to a range request that is not the partial content (206) we requested, we must
    // only transfer the subset of data that WebContent now needs. We discard all received bytes up to the expected
    // start of the remaining data, and then transfer the remaining bytes.
    if (m_start_offset_of_response_resumed_from_cache.has_value()) {
        if (m_status_code == 206) {
            m_start_offset_of_response_resumed_from_cache.clear();
        } else if (m_status_code == 200) {
            // All bytes currently available have already been transferred. Discard them entirely.
            if (m_bytes_transferred_to_client + available_bytes <= *m_start_offset_of_response_resumed_from_cache) {
                m_bytes_transferred_to_client += available_bytes;

                MUST(m_response_buffer.discard(available_bytes));
                return {};
            }

            // Some bytes currently available have already been transferred. Discard those bytes and transfer the rest.
            if (m_bytes_transferred_to_client + available_bytes > *m_start_offset_of_response_resumed_from_cache) {
                auto bytes_to_discard = *m_start_offset_of_response_resumed_from_cache - m_bytes_transferred_to_client;
                m_bytes_transferred_to_client += bytes_to_discard;
                available_bytes -= bytes_to_discard;

                MUST(m_response_buffer.discard(bytes_to_discard));
            }

            m_start_offset_of_response_resumed_from_cache.clear();
        } else {
            return Error::from_string_literal("Unacceptable status code for resumed HTTP request");
        }
    }

    Vector<u8> bytes_to_send;
    bytes_to_send.resize(available_bytes);
    m_response_buffer.peek_some(bytes_to_send);

    auto result = m_client_request_pipe.value().write(bytes_to_send);
    if (result.is_error()) {
        if (result.error().code() != EAGAIN)
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

// IPFS Integration: Start
void Request::set_content_verification_callback(Function<ErrorOr<bool>(ReadonlyBytes)> callback)
{
    m_content_verification_callback = move(callback);
}

void Request::set_gateway_fallback_callback(Function<void()> callback)
{
    m_gateway_fallback_callback = move(callback);
}
// IPFS Integration: End

// Sentinel Security Policy Enforcement: Start

void Request::handle_waiting_for_policy_state()
{
    // Do nothing; we are waiting for the user to make a security decision.
    // The ConnectionFromClient::enforce_security_policy() method will call
    // resume_download(), block_download(), or quarantine_download() based on the user's choice.
}

void Request::resume_download()
{
    dbgln("Request::resume_download: Resuming download for request {}", m_request_id);

    if (m_state != State::WaitingForPolicy) {
        dbgln("Request::resume_download: Warning - request {} is not in WaitingForPolicy state (current state: {})",
              m_request_id, static_cast<int>(m_state));
        return;
    }

    if (!m_curl_easy_handle) {
        dbgln("Request::resume_download: Error - no CURL handle for request {}", m_request_id);
        transition_to_state(State::Error);
        return;
    }

    // Unpause the CURL transfer
    auto result = curl_easy_pause(m_curl_easy_handle, CURLPAUSE_RECV);
    if (result != CURLE_OK) {
        dbgln("Request::resume_download: Failed to unpause CURL transfer: {}", curl_easy_strerror(result));
        transition_to_state(State::Error);
        return;
    }

    // Transition back to Fetch state to continue receiving data
    transition_to_state(State::Fetch);
}

void Request::block_download()
{
    dbgln("Request::block_download: Blocking download for request {}", m_request_id);

    if (m_state != State::WaitingForPolicy) {
        dbgln("Request::block_download: Warning - request {} is not in WaitingForPolicy state (current state: {})",
              m_request_id, static_cast<int>(m_state));
        return;
    }

    // Transition to PolicyBlocked state
    m_state = State::PolicyBlocked;

    // Set network error to indicate the download was blocked
    m_network_error = Requests::NetworkError::Unknown;

    // Abort the CURL transfer
    if (m_curl_easy_handle) {
        auto result = curl_multi_remove_handle(m_curl_multi_handle, m_curl_easy_handle);
        if (result != CURLM_OK)
            dbgln("Request::block_download: Failed to remove CURL handle");

        curl_easy_cleanup(m_curl_easy_handle);
        m_curl_easy_handle = nullptr;
    }

    // Clear the response buffer (delete partial download)
    m_response_buffer = AllocatingMemoryStream();

    // Transition to Complete state to finalize the request
    transition_to_state(State::Complete);
}

void Request::quarantine_download()
{
    dbgln("Request::quarantine_download: Quarantining download for request {}", m_request_id);

    if (m_state != State::WaitingForPolicy) {
        dbgln("Request::quarantine_download: Warning - request {} is not in WaitingForPolicy state (current state: {})",
              m_request_id, static_cast<int>(m_state));
        return;
    }

    // Check if we have a security alert stored
    if (!m_security_alert_json.has_value()) {
        dbgln("Request::quarantine_download: Error - no security alert stored for quarantine");
        transition_to_state(State::Error);
        return;
    }

    // Parse the security alert JSON to extract metadata
    auto json_result = JsonValue::from_string(m_security_alert_json.value());
    if (json_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to parse security alert JSON: {}", json_result.error());
        transition_to_state(State::Error);
        return;
    }

    auto json = json_result.value();
    if (!json.is_object()) {
        dbgln("Request::quarantine_download: Error - security alert JSON is not an object");
        transition_to_state(State::Error);
        return;
    }

    auto obj = json.as_object();

    // Extract metadata from alert JSON
    QuarantineMetadata metadata;

    // Get download metadata
    auto download_metadata = extract_download_metadata();
    metadata.original_url = download_metadata.url;
    metadata.filename = download_metadata.filename;
    metadata.sha256 = download_metadata.sha256;
    metadata.file_size = download_metadata.size_bytes;

    // Get detection time (use current time as ISO 8601)
    auto now = UnixDateTime::now();
    time_t timestamp = now.seconds_since_epoch();
    struct tm tm_buf;
    struct tm* tm_info = gmtime_r(&timestamp, &tm_buf);

    if (tm_info) {
        metadata.detection_time = ByteString::formatted("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
            tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    } else {
        metadata.detection_time = "1970-01-01T00:00:00Z"_string.to_byte_string();
    }

    // Extract rule names from alert JSON
    auto matches = obj.get_array("matches"sv);
    if (matches.has_value()) {
        for (size_t i = 0; i < matches->size(); i++) {
            auto match = matches->at(i);
            if (match.is_object()) {
                auto rule_name = match.as_object().get_string("rule_name"sv);
                if (rule_name.has_value()) {
                    metadata.rule_names.append(rule_name.value().to_byte_string());
                }
            }
        }
    }

    // Write response buffer to a temporary file
    auto buffer_size = m_response_buffer.used_buffer_size();
    if (buffer_size == 0) {
        dbgln("Request::quarantine_download: Error - no content to quarantine");
        transition_to_state(State::Error);
        return;
    }

    // Create temporary file in /tmp
    auto temp_path_result = String::formatted("/tmp/ladybird_quarantine_temp_{}", m_request_id);
    if (temp_path_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to create temp path");
        transition_to_state(State::Error);
        return;
    }
    auto temp_path = temp_path_result.release_value();

    // Read content from response buffer
    auto content_buffer_result = ByteBuffer::create_uninitialized(buffer_size);
    if (content_buffer_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to allocate buffer: {}", content_buffer_result.error());
        transition_to_state(State::Error);
        return;
    }
    auto content_buffer = content_buffer_result.release_value();

    auto read_result = m_response_buffer.read_until_filled(content_buffer);
    if (read_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to read response buffer: {}", read_result.error());
        transition_to_state(State::Error);
        return;
    }

    // Write to temporary file
    auto file_result = Core::File::open(temp_path, Core::File::OpenMode::Write);
    if (file_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to open temp file: {}", file_result.error());
        transition_to_state(State::Error);
        return;
    }
    auto file = file_result.release_value();

    auto write_result = file->write_until_depleted(content_buffer.bytes());
    if (write_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to write to temp file: {}", write_result.error());
        (void)Core::System::unlink(temp_path);
        transition_to_state(State::Error);
        return;
    }

    file->close();

    // Move file to quarantine directory
    auto quarantine_result = Quarantine::quarantine_file(temp_path, metadata);
    if (quarantine_result.is_error()) {
        dbgln("Request::quarantine_download: Error - failed to quarantine file: {}", quarantine_result.error());
        (void)Core::System::unlink(temp_path);
        transition_to_state(State::Error);
        return;
    }

    auto quarantine_id = quarantine_result.release_value();
    dbgln("Request::quarantine_download: Successfully quarantined file with ID: {}", quarantine_id);

    // Transition to Complete state
    transition_to_state(State::Complete);
}

// Sentinel Security Policy Enforcement: End

}
