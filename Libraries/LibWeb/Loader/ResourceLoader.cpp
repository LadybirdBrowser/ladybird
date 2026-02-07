/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/Directory.h>
#include <LibCore/MimeData.h>
#include <LibCore/Resource.h>
#include <LibCore/System.h>
#include <LibGC/Function.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/Loader/ContentFilter.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/LoadRequest.h>
#include <LibWeb/Loader/ProxyMappings.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web {

static RefPtr<ResourceLoader> s_resource_loader;

void ResourceLoader::initialize(GC::Heap& heap, NonnullRefPtr<Requests::RequestClient> request_client)
{
    s_resource_loader = adopt_ref(*new ResourceLoader(heap, move(request_client)));
}

ResourceLoader& ResourceLoader::the()
{
    if (!s_resource_loader) {
        dbgln("Web::ResourceLoader was not initialized");
        VERIFY_NOT_REACHED();
    }
    return *s_resource_loader;
}

ResourceLoader::ResourceLoader(GC::Heap& heap, NonnullRefPtr<Requests::RequestClient> request_client)
    : m_heap(heap)
    , m_request_client(move(request_client))
    , m_user_agent(MUST(String::from_utf8(default_user_agent)))
    , m_platform(MUST(String::from_utf8(default_platform)))
    , m_preferred_languages({ "en-US"_string })
    , m_navigator_compatibility_mode(default_navigator_compatibility_mode)
{
    m_request_client->on_request_server_died = [this]() {
        m_request_client = nullptr;
    };
}

void ResourceLoader::set_client(NonnullRefPtr<Requests::RequestClient> request_client)
{
    m_request_client = move(request_client);
    m_request_client->on_request_server_died = [this]() {
        m_request_client = nullptr;
    };
}

void ResourceLoader::prefetch_dns(URL::URL const& url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (ContentFilter::the().is_filtered(url)) {
        dbgln("ResourceLoader: Refusing to prefetch DNS for '{}': \033[31;1mURL was filtered\033[0m", url);
        return;
    }

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (m_request_client)
        m_request_client->ensure_connection(url, RequestServer::CacheLevel::ResolveOnly);
}

void ResourceLoader::preconnect(URL::URL const& url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (ContentFilter::the().is_filtered(url)) {
        dbgln("ResourceLoader: Refusing to pre-connect to '{}': \033[31;1mURL was filtered\033[0m", url);
        return;
    }

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (m_request_client)
        m_request_client->ensure_connection(url, RequestServer::CacheLevel::CreateConnection);
}

static ByteString sanitized_url_for_logging(URL::URL const& url)
{
    if (url.scheme() == "data"sv)
        return "[data URL]"sv;
    return url.to_byte_string();
}

static void store_response_cookies(Page& page, URL::URL const& url, StringView set_cookie_entry)
{
    auto decoded_cookie = String::from_utf8(set_cookie_entry);
    if (decoded_cookie.is_error())
        return;

    auto cookie = HTTP::Cookie::parse_cookie(url, decoded_cookie.value());
    if (!cookie.has_value())
        return;

    page.client().page_did_set_cookie(url, cookie.value(), HTTP::Cookie::Source::Http);
}

static NonnullRefPtr<HTTP::HeaderList> response_headers_for_file(StringView path, Optional<time_t> const& modified_time)
{
    // For file:// and resource:// URLs, we have to guess the MIME type, since there's no HTTP header to tell us what
    // it is. We insert a fake Content-Type header here, so that clients can use it to learn the MIME type.
    auto mime_type = Core::guess_mime_type_based_on_filename(path);

    auto response_headers = HTTP::HeaderList::create({
        { "Access-Control-Allow-Origin"sv, "null"sv },
        { "Content-Type"sv, mime_type },
    });

    if (modified_time.has_value()) {
        auto const datetime = AK::UnixDateTime::from_seconds_since_epoch(modified_time.value());
        response_headers->set({ "Last-Modified"sv, datetime.to_byte_string("%a, %d %b %Y %H:%M:%S GMT"sv, AK::UnixDateTime::LocalTime::No) });
    }

    return response_headers;
}

static void log_request_start(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());

    dbgln_if(SPAM_DEBUG, "ResourceLoader: Starting load of: \"{}\"", url_for_logging);
}

static void log_success(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());
    auto load_time_ms = request.load_time().to_milliseconds();

    dbgln_if(SPAM_DEBUG, "ResourceLoader: Finished load of: \"{}\", Duration: {}ms", url_for_logging, load_time_ms);
}

template<typename ErrorType>
static void log_failure(LoadRequest const& request, ErrorType const& error)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());
    auto load_time_ms = request.load_time().to_milliseconds();

    dbgln("ResourceLoader: Failed load of: \"{}\", \033[31;1mError: {}\033[0m, Duration: {}ms", url_for_logging, error, load_time_ms);
}

static void log_filtered_request(LoadRequest const& request)
{
    auto url_for_logging = sanitized_url_for_logging(*request.url());
    dbgln("ResourceLoader: Filtered request to: \"{}\"", url_for_logging);
}

static bool should_block_request(LoadRequest const& request)
{
    auto const& url = request.url().value();

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

template<typename FileHandler, typename ErrorHandler>
void ResourceLoader::handle_file_load_request(LoadRequest& request, FileHandler on_file, ErrorHandler on_error)
{
    auto page = request.page();
    if (!page) {
        auto const error_message = ByteString("INTERNAL ERROR: No Page for file scheme request"sv);
        on_error(error_message);
        return;
    }

    auto const& url = request.url().value();

    FileRequest file_request(url.file_path(), [this, request, on_file, on_error, url](ErrorOr<i32> file_or_error) mutable {
        --m_pending_loads;
        if (on_load_counter_change)
            on_load_counter_change();

        if (file_or_error.is_error()) {
            auto const message = ByteString::formatted("{}", file_or_error.error());
            on_error(message);
            return;
        }

        auto const fd = file_or_error.value();

        auto maybe_is_valid_directory = Core::Directory::is_valid_directory(fd);
        if (!maybe_is_valid_directory.is_error() && maybe_is_valid_directory.value()) {
            auto maybe_response = load_file_directory_page(url);
            if (maybe_response.is_error()) {
                auto const message = ByteString::formatted("{}", maybe_response.error());
                on_error(message);
                return;
            }

            FileLoadResult load_result {
                .data = maybe_response.value().bytes(),
                .response_headers = HTTP::HeaderList::create({ { "Content-Type"sv, "text/html"sv } }),
            };
            on_file(load_result);
            return;
        }

        auto st_or_error = Core::System::fstat(fd);
        if (st_or_error.is_error()) {
            on_error(ByteString::formatted("{}", st_or_error.error()));
            return;
        }

        auto maybe_file = Core::File::adopt_fd(fd, Core::File::OpenMode::Read);
        if (maybe_file.is_error()) {
            on_error(ByteString::formatted("{}", maybe_file.error()));
            return;
        }

        auto file = maybe_file.release_value();
        auto maybe_data = file->read_until_eof();
        if (maybe_data.is_error()) {
            on_error(ByteString::formatted("{}", maybe_data.error()));
            return;
        }

        FileLoadResult load_result {
            .data = maybe_data.value().bytes(),
            .response_headers = response_headers_for_file(request.url()->file_path(), st_or_error.value().st_mtime),
        };
        on_file(load_result);
    });

    page->client().request_file(move(file_request));

    ++m_pending_loads;
    if (on_load_counter_change)
        on_load_counter_change();
}

template<typename Callback>
void ResourceLoader::handle_about_load_request(LoadRequest const& request, Callback callback)
{
    auto const& url = request.url().value();

    dbgln_if(SPAM_DEBUG, "Loading about: URL {}", url);

    auto response_headers = HTTP::HeaderList::create({
        { "Content-Type"sv, "text/html; charset=UTF-8"sv },
    });

    // FIXME: Implement timing info for about requests.
    Requests::RequestTimingInfo timing_info {};

    auto serialized_path = URL::percent_decode(url.serialize_path());

    // About version page
    if (serialized_path == "version") {
        auto version_page = MUST(load_about_version_page());
        callback(version_page.bytes(), timing_info, response_headers);
        return;
    }

    // Other about static HTML pages
    auto target_file = ByteString::formatted("{}.html", serialized_path);

    auto about_directory = MUST(Core::Resource::load_from_uri("resource://ladybird/about-pages"_string));
    if (about_directory->children().contains_slow(target_file.view())) {
        auto resource = Core::Resource::load_from_uri(ByteString::formatted("resource://ladybird/about-pages/{}", target_file));
        if (!resource.is_error()) {
            auto const& buffer = resource.value()->data();
            ReadonlyBytes data(buffer.data(), buffer.size());
            callback(data, timing_info, response_headers);
            return;
        }
    }

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(
        m_heap,
        [callback, timing_info, response_headers = move(response_headers)]() mutable {
            auto buffer = ByteString::empty().to_byte_buffer();
            callback(buffer.bytes(), timing_info, response_headers);
        }));
}

template<typename ResourceHandler, typename ErrorHandler>
void ResourceLoader::handle_resource_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error)
{
    auto const& url = request.url().value();

    auto resource = Core::Resource::load_from_uri(url.serialize());
    if (resource.is_error()) {
        on_error(ByteString::formatted("{}", resource.error()));
        return;
    }

    auto resource_value = resource.release_value();

    // When resource URI is a directory use file directory loader to generate response
    if (resource_value->is_directory()) {
        auto directory_url = URL::Parser::basic_parse(resource_value->file_url());
        VERIFY(directory_url.has_value());

        auto maybe_response = load_file_directory_page(directory_url.release_value());
        if (maybe_response.is_error()) {
            on_error(ByteString::formatted("{}", maybe_response.error()));
            return;
        }

        FileLoadResult load_result {
            .data = maybe_response.value().bytes(),
            .response_headers = HTTP::HeaderList::create({ { "Content-Type"sv, "text/html"sv } }),
        };
        on_resource(load_result);
        return;
    }

    // FIXME: Implement timing info for resource requests.
    Requests::RequestTimingInfo timing_info {};

    FileLoadResult load_result {
        .data = resource_value->data(),
        .response_headers = response_headers_for_file(url.file_path(), resource_value->modified_time()),
        .timing_info = timing_info,
    };
    on_resource(load_result);
}

RefPtr<Requests::Request> ResourceLoader::load(LoadRequest& request, GC::Root<OnHeadersReceived> on_headers_received, GC::Root<OnDataReceived> on_data_received, GC::Root<OnComplete> on_complete)
{
    auto const& url = request.url().value();

    log_request_start(request);
    request.start_timer();

    if (should_block_request(request)) {
        on_complete->function()(false, {}, "Request was blocked"sv);
        return nullptr;
    }

    if (url.scheme() == "about"sv) {
        handle_about_load_request(
            request,
            [on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete = move(on_complete), request](ReadonlyBytes data, Requests::RequestTimingInfo const& timing_info, HTTP::HeaderList const& response_headers) {
                log_success(request);
                on_headers_received->function()(response_headers, {}, {});
                on_data_received->function()(data);
                on_complete->function()(true, timing_info, {});
            });
        return nullptr;
    }

    if (url.scheme() == "resource"sv) {
        handle_resource_load_request(
            request,
            [on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete](FileLoadResult const& load_result) {
                on_headers_received->function()(load_result.response_headers, {}, {});
                on_data_received->function()(load_result.data);
                on_complete->function()(true, load_result.timing_info, {});
            },
            [on_complete](ByteString const& message) {
                Requests::RequestTimingInfo fixme_implement_timing_info {};
                on_complete->function()(false, fixme_implement_timing_info, StringView(message));
            });
        return nullptr;
    }

    if (url.scheme() == "file"sv) {
        handle_file_load_request(
            request,
            [request, on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete](FileLoadResult const& load_result) {
                log_success(request);
                on_headers_received->function()(load_result.response_headers, {}, {});
                on_data_received->function()(load_result.data);
                on_complete->function()(true, load_result.timing_info, {});
            },
            [on_complete, request](ByteString const& message) {
                log_failure(request, message);
                on_complete->function()(false, {}, StringView(message));
            });

        return nullptr;
    }

    if (!url.scheme().is_one_of("http"sv, "https"sv)) {
        auto not_implemented_error = ByteString::formatted("Protocol not implemented: {}", url.scheme());
        log_failure(request, not_implemented_error);
        on_complete->function()(false, {}, not_implemented_error);
        return nullptr;
    }

    auto protocol_request = start_network_request(request);
    if (!protocol_request) {
        on_complete->function()(false, {}, "Failed to start network request"sv);
        return nullptr;
    }

    auto protocol_headers_received = [this, on_headers_received = move(on_headers_received), request, request_id = protocol_request->id()](auto const& response_headers, auto status_code, auto const& reason_phrase) {
        handle_network_response_headers(request, response_headers);

        if (auto page = request.page())
            page->client().page_did_receive_network_response_headers(request_id, status_code.value_or(0), reason_phrase, response_headers->headers());

        on_headers_received->function()(response_headers, move(status_code), reason_phrase);
    };

    auto protocol_data_received = [on_data_received = move(on_data_received), request, request_id = protocol_request->id()](auto data) {
        if (auto page = request.page())
            page->client().page_did_receive_network_response_body(request_id, data);
        on_data_received->function()(data);
    };

    auto protocol_complete = [this, on_complete = move(on_complete), request, &protocol_request = *protocol_request](u64 total_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error) {
        finish_network_request(protocol_request);

        if (auto page = request.page())
            page->client().page_did_finish_network_request(protocol_request.id(), total_size, timing_info, network_error);

        if (!network_error.has_value()) {
            log_success(request);
            on_complete->function()(true, timing_info, {});
        } else {
            auto error_description = MUST(String::formatted(
                "Request finished with error: {}",
                network_error_to_string(*network_error)));
            log_failure(request, error_description);
            on_complete->function()(false, timing_info, error_description);
        }
    };

    protocol_request->set_unbuffered_request_callbacks(move(protocol_headers_received), move(protocol_data_received), move(protocol_complete));
    return protocol_request;
}

RefPtr<Requests::Request> ResourceLoader::start_network_request(LoadRequest const& request)
{
    auto proxy = ProxyMappings::the().proxy_for_url(request.url().value());

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (!m_request_client) {
        log_failure(request, "RequestServer is currently unavailable"sv);
        return nullptr;
    }

    auto protocol_request = m_request_client->start_request(request.method(), request.url().value(), request.headers(), request.body(), request.cache_mode(), request.include_credentials(), proxy);
    if (!protocol_request) {
        log_failure(request, "Failed to initiate load"sv);
        return nullptr;
    }

    protocol_request->on_certificate_requested = []() -> Requests::Request::CertificateAndKey {
        return {};
    };

    if (auto page = request.page()) {
        Optional<String> initiator_type_string;
        if (request.initiator_type().has_value())
            initiator_type_string = Fetch::Infrastructure::initiator_type_to_string(request.initiator_type().value()).to_string();
        page->client().page_did_start_network_request(protocol_request->id(), request.url().value(), request.method(), request.headers().headers(), request.body(), move(initiator_type_string));
    }

    ++m_pending_loads;
    if (on_load_counter_change)
        on_load_counter_change();

    m_active_requests.set(*protocol_request);
    return protocol_request;
}

void ResourceLoader::handle_network_response_headers(LoadRequest const& request, HTTP::HeaderList const& response_headers)
{
    if (!request.page())
        return;

    if (request.include_credentials() == HTTP::Cookie::IncludeCredentials::Yes) {
        // From https://fetch.spec.whatwg.org/#concept-http-network-fetch:
        // 15. If includeCredentials is true, then the user agent should parse and store response
        //     `Set-Cookie` headers given request and response.
        for (auto const& [header, value] : response_headers) {
            if (header.equals_ignoring_ascii_case("Set-Cookie"sv)) {
                store_response_cookies(*request.page(), request.url().value(), value);
            }
        }
    }
}

void ResourceLoader::finish_network_request(NonnullRefPtr<Requests::Request> protocol_request)
{
    --m_pending_loads;
    if (on_load_counter_change)
        on_load_counter_change();

    deferred_invoke([this, protocol_request = move(protocol_request)] {
        auto did_remove = m_active_requests.remove(protocol_request);
        VERIFY(did_remove);
    });
}

}
