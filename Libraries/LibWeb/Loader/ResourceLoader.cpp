/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Debug.h>
#include <LibCore/Directory.h>
#include <LibCore/MimeData.h>
#include <LibCore/Resource.h>
#include <LibCore/System.h>
#include <LibGC/Function.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/HSTSPreloadData.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/LoadRequest.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWebCommon/Loader/UserAgent.h>

namespace Web {

static RefPtr<ResourceLoader>& resource_loader()
{
    static RefPtr<ResourceLoader>& resource_loader = *new RefPtr<ResourceLoader>;
    return resource_loader;
}

static Optional<URL::URL>& proxy_autoconfig_url_storage()
{
    static Optional<URL::URL>& proxy_autoconfig_url = *new Optional<URL::URL>;
    return proxy_autoconfig_url;
}

void ResourceLoader::set_proxy_autoconfig_url(URL::URL url)
{
    proxy_autoconfig_url_storage() = move(url);
}

Optional<URL::URL> const& ResourceLoader::proxy_autoconfig_url()
{
    return proxy_autoconfig_url_storage();
}

void ResourceLoader::initialize(GC::Heap& heap)
{
    resource_loader() = adopt_ref(*new ResourceLoader(heap));
}

bool ResourceLoader::is_initialized()
{
    return resource_loader() != nullptr;
}

ResourceLoader& ResourceLoader::the()
{
    if (!resource_loader()) {
        dbgln("Web::ResourceLoader was not initialized");
        VERIFY_NOT_REACHED();
    }
    return *resource_loader();
}

ResourceLoader::ResourceLoader(GC::Heap& heap)
    : m_heap(heap)
    , m_user_agent(MUST(String::from_utf8(default_user_agent)))
    , m_platform(MUST(String::from_utf8(default_platform)))
    , m_preferred_languages(default_preferred_languages())
{
}

NavigatorCompatibilityMode ResourceLoader::s_navigator_compatibility_mode = default_navigator_compatibility_mode;

void ResourceLoader::set_navigator_compatibility_mode(NavigatorCompatibilityMode mode)
{
    s_navigator_compatibility_mode = mode;

    // A script reads two strings to tell engines apart that no navigator attribute covers: the frames of an error's
    // stack, and the source text of a built-in function. Both follow the engine the navigator claims.
    auto& vm = Bindings::main_thread_vm();
    if (mode == NavigatorCompatibilityMode::Chrome)
        vm.set_source_text_syntaxes(JS::VM::StackFrameSyntax::Indented, JS::VM::NativeFunctionSourceSyntax::SingleLine);
    else
        vm.set_source_text_syntaxes(JS::VM::StackFrameSyntax::AtSign, JS::VM::NativeFunctionSourceSyntax::MultiLine);
}

void ResourceLoader::set_client(Optional<Utf16String> const& top_level_site, NonnullRefPtr<Requests::RequestClient> request_client)
{
    request_client->on_request_server_died = [this, top_level_site, disconnected_client = request_client->make_weak_ptr<Requests::RequestClient>()]() {
        auto& client = ensure_client_for(top_level_site);
        if (client.ptr() == disconnected_client.ptr())
            client = nullptr;
    };
    ensure_client_for(top_level_site) = move(request_client);
}

RefPtr<Requests::RequestClient> ResourceLoader::request_client_for(Optional<HTTP::NetworkIsolationKey> const& network_isolation_key) const
{
    if (!network_isolation_key.has_value())
        return m_request_client;
    return request_client_for_top_level_site(network_isolation_key->top_level_site);
}

RefPtr<Requests::RequestClient> ResourceLoader::request_client_for_top_level_site(Optional<Utf16String> const& top_level_site) const
{
    if (top_level_site.has_value()) {
        if (auto it = m_site_request_clients.find(*top_level_site); it != m_site_request_clients.end())
            return it->value;
    }
    return m_request_client;
}

RefPtr<Requests::RequestClient>& ResourceLoader::ensure_client_for(Optional<Utf16String> const& top_level_site)
{
    if (!top_level_site.has_value())
        return m_request_client;
    return m_site_request_clients.ensure(*top_level_site);
}

Vector<Utf16String> ResourceLoader::top_level_sites() const
{
    Vector<Utf16String> top_level_sites;
    for (auto const& entry : m_site_request_clients)
        top_level_sites.append(entry.key);
    return top_level_sites;
}

RefPtr<Requests::RequestClient> ResourceLoader::take_client_for_testing(Optional<Utf16String> const& top_level_site)
{
    return move(ensure_client_for(top_level_site));
}

// Speculation warms up the RequestServer that the document's requests go through.
static Optional<HTTP::NetworkIsolationKey> speculative_network_isolation_key(GC::Ptr<DOM::Document> document)
{
    if (!document)
        return {};
    return Fetch::Infrastructure::determine_the_network_partition_key(document->relevant_settings_object());
}

void ResourceLoader::prefetch_dns(GC::Ptr<DOM::Document> document, URL::URL const& url, URL::URL const& source_url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (!document || document->content_blocking_enabled()) {
        if (ContentBlocker::the().is_filtered(url, source_url, ContentBlocker::ResourceType::Other)) {
            dbgln("ResourceLoader: Refusing to prefetch DNS for '{}': \033[31;1mURL was filtered\033[0m", url);
            return;
        }
    }

    // Speculation would resolve and connect without the proxy that the PAC file would have picked.
    if (proxy_autoconfig_url().has_value())
        return;

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (auto request_client = request_client_for(speculative_network_isolation_key(document)))
        request_client->ensure_connection(url, RequestServer::CacheLevel::ResolveOnly);
}

void ResourceLoader::preconnect(GC::Ptr<DOM::Document> document, URL::URL const& url, URL::URL const& source_url)
{
    if (url.scheme().is_one_of("file"sv, "data"sv))
        return;

    if (!document || document->content_blocking_enabled()) {
        if (ContentBlocker::the().is_filtered(url, source_url, ContentBlocker::ResourceType::Other)) {
            dbgln("ResourceLoader: Refusing to pre-connect to '{}': \033[31;1mURL was filtered\033[0m", url);
            return;
        }
    }

    // Speculation would resolve and connect without the proxy that the PAC file would have picked.
    if (proxy_autoconfig_url().has_value())
        return;

    // FIXME: We could put this request in a queue until the client connection is re-established.
    if (auto request_client = request_client_for(speculative_network_isolation_key(document)))
        request_client->ensure_connection(url, RequestServer::CacheLevel::CreateConnection);
}

static ByteString sanitized_url_for_logging(URL::URL const& url)
{
    if (url.scheme() == "data"sv)
        return "[data URL]"sv;
    return url.to_byte_string();
}

// https://www.rfc-editor.org/rfc/rfc6797#section-8.2
bool ResourceLoader::is_known_hsts_host(Page& page, String const& host)
{
    if (HTTP::HSTSPreloadData::the().is_known_preloaded_hsts_host(host.to_ascii_lowercase()))
        return true;

    return page.client().page_did_is_known_hsts_host(host);
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

    if (auto page = request.page(); !page || page->content_blocking_enabled_for_url(request.top_level_creation_url())) {
        auto source_url = request.source_url().value_or(url);
        if (ContentBlocker::the().is_filtered(url, source_url, request.destination(), request.initiator_type(), request.request_mode())) {
            log_filtered_request(request);
            if (page)
                page->client().page_did_block_resource();
            return true;
        }
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

    FileRequest file_request(url.file_path(), [request, on_file, on_error, url](ErrorOr<i32> file_or_error) mutable {
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

        auto maybe_file = Core::File::adopt_fd(fd, Core::File::OpenMode::Read);
        if (maybe_file.is_error()) {
            on_error(ByteString::formatted("{}", maybe_file.error()));
            return;
        }

        auto file = maybe_file.release_value();

        auto st_or_error = file->stat();
        if (st_or_error.is_error()) {
            on_error(ByteString::formatted("{}", st_or_error.error()));
            return;
        }
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
}

template<typename ResourceHandler, typename ErrorHandler>
void ResourceLoader::handle_about_load_request(LoadRequest const& request, ResourceHandler on_resource, ErrorHandler on_error)
{
    auto const& url = request.url().value();

    dbgln_if(SPAM_DEBUG, "Loading about: URL {}", url);

    auto response_headers = HTTP::HeaderList::create({
        { "Content-Type"sv, "text/html; charset=UTF-8"sv },
    });

    // FIXME: Implement timing info for about requests.
    Requests::RequestTimingInfo timing_info {};

    auto serialized_path = URL::percent_decode(url.serialize_path());
    auto target_file = ByteString::formatted("{}.html", serialized_path);

    auto about_directory = MUST(Core::Resource::load_from_uri("resource://ladybird/about-pages"_string));
    if (about_directory->children().contains_slow(target_file.view())) {
        auto resource = Core::Resource::load_from_uri(ByteString::formatted("resource://ladybird/about-pages/{}", target_file));
        if (!resource.is_error()) {
            auto const& buffer = resource.value()->data();
            ReadonlyBytes data(buffer.data(), buffer.size());
            on_resource(data, timing_info, response_headers);
            return;
        }
    }

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(
        m_heap,
        [on_error, error_message = ByteString::formatted("No such about page: {}", serialized_path)]() {
            on_error(error_message);
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

RefPtr<Requests::Request> ResourceLoader::load(LoadRequest& request, GC::Root<OnHeadersReceived> on_headers_received, GC::Root<OnDataReceived> on_data_received, GC::Root<OnCachedBodyAvailable> on_cached_body_available, GC::Root<OnComplete> on_complete, Requests::RequestClient::TransferLease transfer_lease)
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
            [on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete, request](ReadonlyBytes data, Requests::RequestTimingInfo const& timing_info, HTTP::HeaderList const& response_headers) {
                log_success(request);
                on_headers_received->function()(nullptr, response_headers, {}, {}, {}, {}, Requests::CacheState::NotCached);
                on_data_received->function()(Requests::ResponseData::from_bytes(data));
                on_complete->function()(true, timing_info, {});
            },
            [on_complete, request](ByteString const& message) {
                log_failure(request, message);
                Requests::RequestTimingInfo fixme_implement_timing_info {};
                on_complete->function()(false, fixme_implement_timing_info, StringView(message));
            });
        return nullptr;
    }

    if (url.scheme() == "resource"sv) {
        handle_resource_load_request(
            request,
            [on_headers_received = move(on_headers_received), on_data_received = move(on_data_received), on_complete](FileLoadResult const& load_result) {
                on_headers_received->function()(nullptr, load_result.response_headers, {}, {}, {}, {}, Requests::CacheState::NotCached);
                on_data_received->function()(Requests::ResponseData::from_bytes(load_result.data));
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
                on_headers_received->function()(nullptr, load_result.response_headers, {}, {}, {}, {}, Requests::CacheState::NotCached);
                on_data_received->function()(Requests::ResponseData::from_bytes(load_result.data));
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

    // We cannot evaluate PAC files yet, and connecting directly would ignore the proxy that was asked for.
    if (proxy_autoconfig_url().has_value()) {
        dbgln("FIXME: Evaluate the PAC file to find the proxy for {}; failing the request", url);
        auto error = "PAC proxy configurations are not supported"sv;
        log_failure(request, error);
        on_complete->function()(false, {}, error);
        return nullptr;
    }

    auto protocol_request = start_network_request(request, transfer_lease);
    if (!protocol_request) {
        on_complete->function()(false, {}, "Failed to start network request"sv);
        return nullptr;
    }

    auto protocol_headers_received = [on_headers_received = move(on_headers_received), request, &protocol_request = *protocol_request](auto const& response_headers, auto status_code, auto const& reason_phrase, auto javascript_bytecode, auto javascript_bytecode_cache_vary_key, auto cache_state) {
        if (auto page = request.page())
            page->client().page_did_receive_network_response_headers(protocol_request.id(), status_code.value_or(0), reason_phrase, response_headers->headers(), cache_state);

        on_headers_received->function()(&protocol_request, response_headers, move(status_code), reason_phrase, move(javascript_bytecode), javascript_bytecode_cache_vary_key, cache_state);
    };

    auto protocol_data_received = [on_data_received = move(on_data_received), request, request_id = protocol_request->id()](auto data) {
        if (auto page = request.page())
            page->client().page_did_receive_network_response_body(request_id, data.bytes());
        on_data_received->function()(move(data));
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

    auto protocol_cached_body_available = [on_cached_body_available = move(on_cached_body_available)](auto data) {
        on_cached_body_available->function()(move(data));
    };

    protocol_request->set_unbuffered_request_callbacks(move(protocol_headers_received), move(protocol_data_received), move(protocol_cached_body_available), move(protocol_complete));
    protocol_request->set_stop_callback([this, &protocol_request = *protocol_request] {
        finish_network_request(protocol_request);
    });
    return protocol_request;
}

static NonnullRefPtr<HTTP::HeaderList const> headers_with_normalized_values(HTTP::HeaderList const& headers)
{
    if (all_of(headers, [](auto const& header) { return HTTP::is_header_value(header.value); }))
        return headers;

    auto normalized_headers = HTTP::HeaderList::create();
    for (auto const& header : headers)
        normalized_headers->append_with_normalized_name({ header.name, HTTP::normalize_header_value(header.value) });
    return normalized_headers;
}

RefPtr<Requests::Request> ResourceLoader::start_network_request(LoadRequest const& request, Requests::RequestClient::TransferLease transfer_lease)
{
    // FIXME: We could put this request in a queue until the client connection is re-established.
    auto request_client = request_client_for(request.network_isolation_key());
    if (!request_client) {
        log_failure(request, "RequestServer is currently unavailable"sv);
        return nullptr;
    }

    auto cache_miss_notification = request.destination() == Fetch::Infrastructure::Request::Destination::Font
        ? Requests::RequestClient::CacheMissNotification::Yes
        : Requests::RequestClient::CacheMissNotification::No;
    // NB: Workers have a Page shim without a navigable or a browser page ID. Their
    //     requests are attributed through the worker process's browser-level owners.
    auto originating_page_id = request.page() && request.page()->has_top_level_traversable() ? request.page()->client().id().value() : 0;
    auto headers = headers_with_normalized_values(request.headers());
    auto protocol_request = request_client->start_request(request.method(), request.url().value(), *headers, request.body(), request.cache_mode(), request.include_credentials(), transfer_lease, {}, cache_miss_notification, originating_page_id, request.network_isolation_key());
    if (!protocol_request) {
        log_failure(request, "Failed to initiate load"sv);
        return nullptr;
    }

    if (auto page = request.page()) {
        Optional<String> initiator_type_string;
        if (request.initiator_type().has_value())
            initiator_type_string = MUST(Fetch::Infrastructure::initiator_type_to_string(request.initiator_type().value()).view().to_utf8());
        auto referrer_policy = MUST(String::from_utf8(ReferrerPolicy::to_string(request.referrer_policy())));
        page->client().page_did_start_network_request(protocol_request->id(), request.url().value(), request.method(), request.headers().headers(), request.body(), move(initiator_type_string), referrer_policy, request.is_navigation_request(), request.priority());
    }

    m_active_requests.set(*protocol_request);
    return protocol_request;
}

void ResourceLoader::finish_network_request(NonnullRefPtr<Requests::Request> protocol_request)
{
    if (!m_active_requests.remove(protocol_request))
        warnln("ResourceLoader: finish_network_request() called for request {}, which is not active", protocol_request->id());
}

}
