/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/DateTime.h>
#include <LibCore/Directory.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/MimeData.h>
#include <LibCore/Resource.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/Loader/ContentFilter.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/LoadRequest.h>
#include <LibWeb/Loader/ProxyMappings.h>
#include <LibWeb/Loader/Resource.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web {

ResourceLoaderConnectorRequest::ResourceLoaderConnectorRequest() = default;

ResourceLoaderConnectorRequest::~ResourceLoaderConnectorRequest() = default;

ResourceLoaderConnector::ResourceLoaderConnector() = default;

ResourceLoaderConnector::~ResourceLoaderConnector() = default;

static RefPtr<ResourceLoader> s_resource_loader;

void ResourceLoader::initialize(RefPtr<ResourceLoaderConnector> connector)
{
    if (connector)
        s_resource_loader = ResourceLoader::try_create(connector.release_nonnull()).release_value_but_fixme_should_propagate_errors();
}

ResourceLoader& ResourceLoader::the()
{
    if (!s_resource_loader) {
        dbgln("Web::ResourceLoader was not initialized");
        VERIFY_NOT_REACHED();
    }
    return *s_resource_loader;
}

ErrorOr<NonnullRefPtr<ResourceLoader>> ResourceLoader::try_create(NonnullRefPtr<ResourceLoaderConnector> connector)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) ResourceLoader(move(connector)));
}

ResourceLoader::ResourceLoader(NonnullRefPtr<ResourceLoaderConnector> connector)
    : m_connector(move(connector))
    , m_user_agent(MUST(String::from_utf8(default_user_agent)))
    , m_platform(MUST(String::from_utf8(default_platform)))
    , m_navigator_compatibility_mode(default_navigator_compatibility_mode)
{
}

void ResourceLoader::prefetch_dns(URL::URL const& url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (ContentFilter::the().is_filtered(url)) {
        dbgln("ResourceLoader: Refusing to prefetch DNS for '{}': \033[31;1mURL was filtered\033[0m", url);
        return;
    }

    m_connector->prefetch_dns(url);
}

void ResourceLoader::preconnect(URL::URL const& url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (ContentFilter::the().is_filtered(url)) {
        dbgln("ResourceLoader: Refusing to pre-connect to '{}': \033[31;1mURL was filtered\033[0m", url);
        return;
    }

    m_connector->preconnect(url);
}

static HashMap<LoadRequest, NonnullRefPtr<Resource>> s_resource_cache;

RefPtr<Resource> ResourceLoader::load_resource(Resource::Type type, LoadRequest& request)
{
    if (!request.is_valid())
        return nullptr;

    bool use_cache = request.url().scheme() != "file";

    if (use_cache) {
        auto it = s_resource_cache.find(request);
        if (it != s_resource_cache.end()) {
            if (it->value->type() != type) {
                dbgln("FIXME: Not using cached resource for {} since there's a type mismatch.", request.url());
            } else {
                dbgln_if(CACHE_DEBUG, "Reusing cached resource for: {}", request.url());
                return it->value;
            }
        }
    }

    auto resource = Resource::create({}, type, request);

    if (use_cache)
        s_resource_cache.set(request, resource);

    load(
        request,
        [=](auto data, auto& headers, auto status_code) {
            const_cast<Resource&>(*resource).did_load({}, data, headers, status_code);
        },
        [=](auto& error, auto status_code, auto, auto) {
            const_cast<Resource&>(*resource).did_fail({}, error, status_code);
        });

    return resource;
}

static ByteString sanitized_url_for_logging(URL::URL const& url)
{
    if (url.scheme() == "data"sv)
        return "[data URL]"sv;
    return url.to_byte_string();
}

static void store_response_cookies(Page& page, URL::URL const& url, ByteString const& set_cookie_entry)
{
    auto cookie = Cookie::parse_cookie(set_cookie_entry);
    if (!cookie.has_value())
        return;
    page.client().page_did_set_cookie(url, cookie.value(), Cookie::Source::Http); // FIXME: Determine cookie source correctly
}

static HTTP::HeaderMap response_headers_for_file(StringView path, Optional<time_t> const& modified_time)
{
    // For file:// and resource:// URLs, we have to guess the MIME type, since there's no HTTP header to tell us what
    // it is. We insert a fake Content-Type header here, so that clients can use it to learn the MIME type.
    auto mime_type = Core::guess_mime_type_based_on_filename(path);

    HTTP::HeaderMap response_headers;
    response_headers.set("Content-Type"sv, mime_type);

    if (modified_time.has_value()) {
        auto const datetime = Core::DateTime::from_timestamp(modified_time.value());
        response_headers.set("Last-Modified"sv, datetime.to_byte_string("%a, %d %b %Y %H:%M:%S GMT"sv, Core::DateTime::LocalTime::No));
    }

    return response_headers;
}

static void log_request_start(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(request.url());

    dbgln_if(SPAM_DEBUG, "ResourceLoader: Starting load of: \"{}\"", url_for_logging);
}

static void log_success(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(request.url());
    auto load_time_ms = request.load_time().to_milliseconds();

    dbgln_if(SPAM_DEBUG, "ResourceLoader: Finished load of: \"{}\", Duration: {}ms", url_for_logging, load_time_ms);
}

template<typename ErrorType>
static void log_failure(LoadRequest const& request, ErrorType const& error)
{
    auto url_for_logging = sanitized_url_for_logging(request.url());
    auto load_time_ms = request.load_time().to_milliseconds();

    dbgln("ResourceLoader: Failed load of: \"{}\", \033[31;1mError: {}\033[0m, Duration: {}ms", url_for_logging, error, load_time_ms);
}

static void log_filtered_request(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(request.url());
    dbgln("ResourceLoader: Filtered request to: \"{}\"", url_for_logging);
}

static bool should_block_request(LoadRequest const& request)
{
    auto const& url = request.url();

    auto is_port_blocked = [](int port) {
        static constexpr auto ports = to_array({ 1, 7, 9, 11, 13, 15, 17, 19, 20, 21, 22, 23, 25, 37, 42,
            43, 53, 77, 79, 87, 95, 101, 102, 103, 104, 109, 110, 111, 113, 115, 117, 119, 123, 135, 139,
            143, 179, 389, 465, 512, 513, 514, 515, 526, 530, 531, 532, 540, 556, 563, 587, 601, 636,
            993, 995, 2049, 3659, 4045, 6000, 6379, 6665, 6666, 6667, 6668, 6669 });

        return ports.first_index_of(port).has_value();
    };

    if (is_port_blocked(url.port_or_default())) {
        log_failure(request, ByteString::formatted("Port #{} is blocked", url.port_or_default()));
        return true;
    }

    if (ContentFilter::the().is_filtered(url)) {
        log_filtered_request(request);
        return true;
    }

    return false;
}

void ResourceLoader::load(LoadRequest& request, SuccessCallback success_callback, ErrorCallback error_callback, Optional<u32> timeout, TimeoutCallback timeout_callback)
{
    auto const& url = request.url();

    log_request_start(request);
    request.start_timer();

    if (should_block_request(request)) {
        error_callback("Request was blocked", {}, {}, {});
        return;
    }

    auto respond_directory_page = [](LoadRequest const& request, URL::URL const& url, SuccessCallback const& success_callback, ErrorCallback const& error_callback) {
        auto maybe_response = load_file_directory_page(url);
        if (maybe_response.is_error()) {
            log_failure(request, maybe_response.error());
            if (error_callback)
                error_callback(ByteString::formatted("{}", maybe_response.error()), 500u, {}, {});
            return;
        }

        log_success(request);
        HTTP::HeaderMap response_headers;
        response_headers.set("Content-Type"sv, "text/html"sv);
        success_callback(maybe_response.release_value().bytes(), response_headers, {});
    };

    if (url.scheme() == "about") {
        dbgln_if(SPAM_DEBUG, "Loading about: URL {}", url);
        log_success(request);

        HTTP::HeaderMap response_headers;
        response_headers.set("Content-Type", "text/html; charset=UTF-8");

        // About version page
        if (url.path_segment_at_index(0) == "version") {
            success_callback(MUST(load_about_version_page()).bytes(), response_headers, {});
            return;
        }

        // Other about static HTML pages
        auto resource = Core::Resource::load_from_uri(MUST(String::formatted("resource://ladybird/{}.html", url.path_segment_at_index(0))));
        if (!resource.is_error()) {
            auto data = resource.value()->data();
            success_callback(data, response_headers, {});
            return;
        }

        Platform::EventLoopPlugin::the().deferred_invoke([success_callback = move(success_callback), response_headers = move(response_headers)] {
            success_callback(ByteString::empty().to_byte_buffer(), response_headers, {});
        });
        return;
    }

    if (url.scheme() == "data") {
        auto data_url_or_error = Fetch::Infrastructure::process_data_url(url);
        if (data_url_or_error.is_error()) {
            auto error_message = data_url_or_error.error().string_literal();
            log_failure(request, error_message);
            error_callback(error_message, {}, {}, {});
            return;
        }
        auto data_url = data_url_or_error.release_value();

        dbgln_if(SPAM_DEBUG, "ResourceLoader loading a data URL with mime-type: '{}', payload='{}'",
            MUST(data_url.mime_type.serialized()),
            StringView(data_url.body.bytes()));

        HTTP::HeaderMap response_headers;
        response_headers.set("Content-Type", MUST(data_url.mime_type.serialized()).to_byte_string());

        log_success(request);

        Platform::EventLoopPlugin::the().deferred_invoke([data = move(data_url.body), response_headers = move(response_headers), success_callback = move(success_callback)] {
            success_callback(data, response_headers, {});
        });
        return;
    }

    if (url.scheme() == "resource") {
        auto resource = Core::Resource::load_from_uri(url.serialize());
        if (resource.is_error()) {
            log_failure(request, resource.error());
            if (error_callback)
                error_callback(ByteString::formatted("{}", resource.error()), {}, {}, {});
            return;
        }

        // When resource URI is a directory use file directory loader to generate response
        if (resource.value()->is_directory()) {
            respond_directory_page(request, resource.value()->file_url(), success_callback, error_callback);
            return;
        }

        auto data = resource.value()->data();
        auto response_headers = response_headers_for_file(url.serialize_path(), resource.value()->modified_time());

        log_success(request);
        success_callback(data, response_headers, {});

        return;
    }

    if (url.scheme() == "file") {
        if (request.page())
            m_page = request.page();

        if (!m_page.has_value()) {
            log_failure(request, "INTERNAL ERROR: No Page for request");
            return;
        }

        FileRequest file_request(url.serialize_path(), [this, success_callback = move(success_callback), error_callback = move(error_callback), request, respond_directory_page](ErrorOr<i32> file_or_error) {
            --m_pending_loads;
            if (on_load_counter_change)
                on_load_counter_change();

            if (file_or_error.is_error()) {
                log_failure(request, file_or_error.error());
                if (error_callback)
                    error_callback(ByteString::formatted("{}", file_or_error.error()), {}, {}, {});
                return;
            }

            auto const fd = file_or_error.value();

            // When local file is a directory use file directory loader to generate response
            auto maybe_is_valid_directory = Core::Directory::is_valid_directory(fd);
            if (!maybe_is_valid_directory.is_error() && maybe_is_valid_directory.value()) {
                respond_directory_page(request, request.url(), success_callback, error_callback);
                return;
            }

            auto st_or_error = Core::System::fstat(fd);
            if (st_or_error.is_error()) {
                log_failure(request, st_or_error.error());
                if (error_callback)
                    error_callback(ByteString::formatted("{}", st_or_error.error()), {}, {}, {});
                return;
            }

            // Try to read file normally
            auto maybe_file = Core::File::adopt_fd(fd, Core::File::OpenMode::Read);
            if (maybe_file.is_error()) {
                log_failure(request, maybe_file.error());
                if (error_callback)
                    error_callback(ByteString::formatted("{}", maybe_file.error()), {}, {}, {});
                return;
            }

            auto file = maybe_file.release_value();
            auto maybe_data = file->read_until_eof();
            if (maybe_data.is_error()) {
                log_failure(request, maybe_data.error());
                if (error_callback)
                    error_callback(ByteString::formatted("{}", maybe_data.error()), {}, {}, {});
                return;
            }

            auto data = maybe_data.release_value();
            auto response_headers = response_headers_for_file(request.url().serialize_path(), st_or_error.value().st_mtime);

            log_success(request);
            success_callback(data, response_headers, {});
        });

        (*m_page)->client().request_file(move(file_request));

        ++m_pending_loads;
        if (on_load_counter_change)
            on_load_counter_change();

        return;
    }

    if (url.scheme() == "http" || url.scheme() == "https") {
        auto protocol_request = start_network_request(request);
        if (!protocol_request) {
            if (error_callback)
                error_callback("Failed to start network request"sv, {}, {}, {});
            return;
        }

        if (timeout.has_value() && timeout.value() > 0) {
            auto timer = Platform::Timer::create_single_shot(timeout.value(), nullptr);
            timer->on_timeout = [timer, protocol_request, timeout_callback = move(timeout_callback)] {
                protocol_request->stop();
                if (timeout_callback)
                    timeout_callback();
            };
            timer->start();
        }

        auto on_buffered_request_finished = [this, success_callback = move(success_callback), error_callback = move(error_callback), request, &protocol_request = *protocol_request](bool success, auto, auto& response_headers, auto status_code, ReadonlyBytes payload) mutable {
            handle_network_response_headers(request, response_headers);
            finish_network_request(protocol_request);

            if (!success || (status_code.has_value() && *status_code >= 400 && *status_code <= 599 && (payload.is_empty() || !request.is_main_resource()))) {
                StringBuilder error_builder;
                if (status_code.has_value())
                    error_builder.appendff("Load failed: {}", *status_code);
                else
                    error_builder.append("Load failed"sv);
                log_failure(request, error_builder.string_view());
                if (error_callback)
                    error_callback(error_builder.to_byte_string(), status_code, payload, response_headers);
                return;
            }

            log_success(request);
            success_callback(payload, response_headers, status_code);
        };

        protocol_request->set_buffered_request_finished_callback(move(on_buffered_request_finished));
        return;
    }

    auto not_implemented_error = ByteString::formatted("Protocol not implemented: {}", url.scheme());
    log_failure(request, not_implemented_error);
    if (error_callback)
        error_callback(not_implemented_error, {}, {}, {});
}

void ResourceLoader::load_unbuffered(LoadRequest& request, OnHeadersReceived on_headers_received, OnDataReceived on_data_received, OnComplete on_complete)
{
    auto const& url = request.url();

    log_request_start(request);
    request.start_timer();

    if (should_block_request(request)) {
        on_complete(false, "Request was blocked"sv);
        return;
    }

    if (!url.scheme().is_one_of("http"sv, "https"sv)) {
        // FIXME: Non-network requests from fetch should not go through this path.
        on_complete(false, "Cannot establish connection non-network scheme"sv);
        return;
    }

    auto protocol_request = start_network_request(request);
    if (!protocol_request) {
        on_complete(false, "Failed to start network request"sv);
        return;
    }

    auto protocol_headers_received = [this, on_headers_received = move(on_headers_received), request](auto const& response_headers, auto status_code) {
        handle_network_response_headers(request, response_headers);
        on_headers_received(response_headers, move(status_code));
    };

    auto protocol_data_received = [on_data_received = move(on_data_received)](auto data) {
        on_data_received(data);
    };

    auto protocol_complete = [this, on_complete = move(on_complete), request, &protocol_request = *protocol_request](bool success, u64) {
        finish_network_request(protocol_request);

        if (success) {
            log_success(request);
            on_complete(true, {});
        } else {
            log_failure(request, "Request finished with error"sv);
            on_complete(false, "Request finished with error"sv);
        }
    };

    protocol_request->set_unbuffered_request_callbacks(move(protocol_headers_received), move(protocol_data_received), move(protocol_complete));
}

RefPtr<ResourceLoaderConnectorRequest> ResourceLoader::start_network_request(LoadRequest const& request)
{
    auto proxy = ProxyMappings::the().proxy_for_url(request.url());

    HTTP::HeaderMap headers;

    for (auto const& it : request.headers()) {
        headers.set(it.key, it.value);
    }

    if (!headers.contains("User-Agent"))
        headers.set("User-Agent", m_user_agent.to_byte_string());

    if (!headers.contains("Accept-Encoding"))
        headers.set("Accept-Encoding", "gzip, deflate, br");

    auto protocol_request = m_connector->start_request(request.method(), request.url(), headers, request.body(), proxy);
    if (!protocol_request) {
        log_failure(request, "Failed to initiate load"sv);
        return nullptr;
    }

    protocol_request->on_certificate_requested = []() -> ResourceLoaderConnectorRequest::CertificateAndKey {
        return {};
    };

    ++m_pending_loads;
    if (on_load_counter_change)
        on_load_counter_change();

    m_active_requests.set(*protocol_request);
    return protocol_request;
}

void ResourceLoader::handle_network_response_headers(LoadRequest const& request, HTTP::HeaderMap const& response_headers)
{
    if (!request.page())
        return;

    for (auto const& [header, value] : response_headers.headers()) {
        if (header.equals_ignoring_ascii_case("Set-Cookie"sv)) {
            store_response_cookies(*request.page(), request.url(), value);
        }
    }

    if (auto cache_control = response_headers.get("Cache-Control"); cache_control.has_value()) {
        if (cache_control.value().contains("no-store"sv))
            s_resource_cache.remove(request);
    }
}

void ResourceLoader::finish_network_request(NonnullRefPtr<ResourceLoaderConnectorRequest> const& protocol_request)
{
    --m_pending_loads;
    if (on_load_counter_change)
        on_load_counter_change();

    Platform::EventLoopPlugin::the().deferred_invoke([this, protocol_request] {
        m_active_requests.remove(protocol_request);
    });
}

void ResourceLoader::clear_cache()
{
    dbgln_if(CACHE_DEBUG, "Clearing {} items from ResourceLoader cache", s_resource_cache.size());
    s_resource_cache.clear();
}

void ResourceLoader::evict_from_cache(LoadRequest const& request)
{
    dbgln_if(CACHE_DEBUG, "Removing resource {} from cache", request.url());
    s_resource_cache.remove(request);
}

}
