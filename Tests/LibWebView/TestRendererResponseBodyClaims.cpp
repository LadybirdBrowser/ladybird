/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibGfx/SystemTheme.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/Page/NavigationTarget.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigation.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/RequestServerSiteBindings.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WorkerAgentManager.h>

namespace {

constexpr auto s_response_body = "The body of a navigation's response."sv;

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

class TestFixture {
public:
    TestFixture()
        : m_environment("TestRendererResponseBodyClaims"sv)
        , m_application(MUST(m_environment.create_application<TestApplication>()))
        , m_theme(MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string())))
    {
    }

    NonnullOwnPtr<WebView::HeadlessWebView> create_view() const { return WebView::HeadlessWebView::create(m_theme, { 800, 600 }); }

    TestApplication& application() { return *m_application; }
    ByteString const& directory() const { return m_environment.directory(); }

private:
    Test::HeadlessTestEnvironment m_environment;
    NonnullOwnPtr<TestApplication> m_application;
    Core::AnonymousBuffer m_theme;
};

// Sends every connection a response up to the given length of its body. The request stays in flight, and leased, until
// the test sends the rest. RequestServer passes on a response's headers once its body starts.
class TestResponseServer {
public:
    TestResponseServer()
        : TestResponseServer("application/octet-stream"sv, s_response_body, 1)
    {
    }

    TestResponseServer(StringView content_type, StringView body, size_t sent_length)
        : m_start_of_response(ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", content_type, body.length(), body.substring_view(0, sent_length)))
        , m_rest_of_response(body.substring_view(sent_length))
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto connection = MUST(m_server->accept());
            MUST(connection->write_until_depleted(m_start_of_response.bytes()));
            m_connections.append(move(connection));
        };
    }

    // A separate URL for each request, as a request can wait for an earlier request for its URL.
    URL::URL unique_url()
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/{}", *m_server->local_port(), m_next_url_path++)).release_value();
    }

    void send_rest_of_last_response() { MUST(m_connections.last()->write_until_depleted(m_rest_of_response.bytes())); }

private:
    ByteString m_start_of_response;
    ByteString m_rest_of_response;
    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Core::TCPSocket>> m_connections;
    u64 m_next_url_path { 0 };
};

class FinishedDownloadRecorder final : public WebView::FileDownloaderObserver {
public:
    virtual void download_updated(WebView::FileDownloader::Download const& download) override
    {
        if (!WebView::FileDownloader::status_is_active(download.status))
            m_finished = download;
    }

    Optional<WebView::FileDownloader::Download> const& finished() const { return m_finished; }

private:
    Optional<WebView::FileDownloader::Download> m_finished;
};

Web::HTML::NavigationPopulationResult result_with_response_body_of(int request_server_client_id, u64 request_server_request_id, Optional<Utf16String> top_level_site = {})
{
    auto url = URL::Parser::basic_parse("https://victim.example/"sv).release_value();
    Web::HTML::NavigationParamsDescriptor navigation_params {
        .id = {},
        .navigable_id = {},
        .request = {},
        .response = {},
        .fetch_timing_info = {},
        .coop_enforcement_result = { .url = url, .origin = url.origin(), .opener_policy = {} },
        .reserved_environment = {},
        .origin = url.origin(),
        .policy_container = {},
        .opener_policy = {},
        .about_base_url = {},
        .agent_cluster_id = {},
        .new_browsing_context_group_id = {},
    };
    navigation_params.response.body = Web::HTML::NavigationResponseBodyHandle {
        .request_server_client_id = request_server_client_id,
        .request_server_request_id = request_server_request_id,
        .request_server_top_level_site = move(top_level_site),
    };
    return {
        .navigation_params = move(navigation_params),
        .redirected_url = {},
        .classic_history_api_state = {},
        .replacement_document_state = {},
    };
}

Web::HTML::NavigationPopulationRequest population_request_for(WebView::HeadlessWebView& view, URL::URL const& url, Utf16String const& navigation_id)
{
    Web::HTML::NavigationPopulationRequest request {
        .navigable_id = view.traversable().id(),
        .history_entry = {},
        .source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document(),
        .target_snapshot_params = {},
        .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
        .history_handling = Web::Bindings::NavigationHistoryBehavior::Auto,
        .user_involvement = Web::HTML::UserNavigationInvolvement::None,
        .navigation_id = navigation_id,
    };
    request.history_entry.url = url;
    return request;
}

// Replaces the view's client for the site as its renderer does, and holds the new connection in the renderer's place.
// The renderer's earlier connection stays open.
NonnullRefPtr<Requests::RequestClient> connect_client(WebView::HeadlessWebView& view, Optional<Utf16String> top_level_site = {})
{
    auto& stub = static_cast<WebContentClientStub&>(view.page().client());
    auto handle = stub.did_lose_request_server_connection(move(top_level_site)).take_handle();
    return make_ref_counted<Requests::RequestClient>(MUST(handle->create_transport()));
}

// Returns once RequestServer holds the request's lease, as other connections reach it in their own time. The body stays
// unread, for the client that adopts it last.
NonnullRefPtr<Requests::Request> start_leased_request(Requests::RequestClient& client, URL::URL const& url)
{
    auto request = client.start_request("GET"sv, url, HTTP::Cookie::SameSiteContext::same_site(), {}, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, Requests::RequestClient::TransferLease::Yes);
    request->set_body_delivery_paused(true);
    (void)client.send_sync<Messages::RequestServer::GetClientId>();
    return request.release_nonnull();
}

// Starts a navigation to about:blank, as far as the UI process waiting for its navigation params.
Utf16String start_navigation(WebView::HeadlessWebView& view)
{
    auto& stub = static_cast<WebContentClientStub&>(view.page().client());
    auto page_id = view.page().id();
    auto navigable_id = view.traversable().id();
    auto navigation_id = Utf16String::from_utf8(generate_random_uuid());
    Web::HTML::NavigationStartRequest start_request {
        .navigable_id = navigable_id,
        .url = URL::about_blank(),
        .document_resource = {},
        .request_referrer = Web::Fetch::Infrastructure::RequestReferrer::Client,
        .request_referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
        .initiator_origin = URL::Origin::create_opaque(),
        .initiator_base_url = {},
        .navigable_target_name = {},
        .source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document(),
        .target_snapshot_params = {},
        .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
        .history_handling = Web::Bindings::NavigationHistoryBehavior::Replace,
        .user_involvement = Web::HTML::UserNavigationInvolvement::None,
        .navigation_id = navigation_id,
        .classic_history_api_state = {},
        .navigation_api_state = {},
        .navigation_api_key = {},
        .navigation_api_id = {},
        .directive_state_id = {},
        .directive_state_value = {},
    };
    stub.did_request_navigation_start(page_id, navigable_id, Web::NavigationTarget::TopLevel, URL::about_blank(), navigation_id, move(start_request));
    stub.did_complete_navigation_unload_check(page_id, navigable_id, navigation_id);
    VERIFY(view.traversable().navigation_with_id(navigation_id)->state.has<WebView::CanonicalNavigation::CreatingNavigationParams>());
    return navigation_id;
}

void load_page(WebView::HeadlessWebView& view, URL::URL const& url)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA bool loaded = false;
    view.on_load_finish = [&](URL::URL const&) { loaded = true; };
    view.load(url);
    Core::EventLoop::current().spin_until([&] { return loaded; });
    view.on_load_finish = nullptr;
}

void expect_rejected(WebView::HeadlessWebView& view, StringView what, Function<void()> const& send)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<WebView::ViewImplementation::WebContentCrashReason> crash_reason;
    view.on_web_content_crashed = [&](auto reason) { crash_reason = reason; };

    send();
    Core::EventLoop::current().spin_until([&] { return crash_reason.has_value(); });
    view.on_web_content_crashed = nullptr;

    if (crash_reason != WebView::ViewImplementation::WebContentCrashReason::RejectedIPC) {
        warnln("FAIL: {} was not rejected", what);
        VERIFY_NOT_REACHED();
    }
}

// A navigation of the view in which the test acts for the processes running it.
class TestPopulation {
public:
    TestPopulation(WebView::HeadlessWebView& view, URL::URL url)
        : m_view(view)
        , m_url(move(url))
        , m_navigation_id(Utf16String::from_utf8(generate_random_uuid()))
        , m_loader(WebView::NavigationLoader::create(population_request_for(view, m_url, m_navigation_id)))
    {
        view.traversable().set_ongoing_navigation({
            .url = m_url,
            .navigation_id = m_navigation_id,
            .state = WebView::CanonicalNavigation::CreatingNavigationParams { m_loader },
            .page = &view.page(),
        });
    }

    WebView::NavigationLoader& loader() { return *m_loader; }
    bool is_in_flight() const { return m_view.traversable().navigation_with_id(m_navigation_id) != nullptr; }
    Requests::RequestTransferLeaseKey transfer_lease() const { return m_loader->response_body_transfer_lease().value(); }

    // Fetches the response through the client, and has the UI process acquire its body.
    void fetch_response_through(Requests::RequestClient& client, Optional<Utf16String> top_level_site = {})
    {
        m_fetch = start_leased_request(client, m_url);
        m_fetch->set_unbuffered_request_callbacks(
            [this](auto, auto, auto const&, auto) { m_received_response_headers = true; },
            [](auto) { },
            [](auto) { },
            [](auto, auto const&, auto) { });
        Core::EventLoop::current().spin_until([&] { return m_received_response_headers; });

        m_loader->did_finish_navigation_params_creation(result_with_response_body_of(client.request_server_client_id(), m_fetch->id(), move(top_level_site)));

        Optional<bool> acquired;
        m_loader->acquire_response_body(m_view.page().client(), [&](bool succeeded) { acquired = succeeded; });
        Core::EventLoop::current().spin_until([&] { return acquired.has_value(); });
        VERIFY(*acquired);

        m_view.traversable().navigation_with_id(m_navigation_id)->state = WebView::CanonicalNavigation::Populating { m_loader };
    }

    void move_response_body_to(WebView::HeadlessWebView const& host_view)
    {
        Optional<bool> moved;
        m_loader->move_response_body_to(host_view.page().client().request_server_site_bindings(), [&](bool succeeded) { moved = succeeded; });
        Core::EventLoop::current().spin_until([&] { return moved.has_value(); });
        VERIFY(*moved);
    }

    void hand_response_body_to(WebView::HeadlessWebView& host_view)
    {
        m_view.traversable().navigation_with_id(m_navigation_id)->page = &host_view.page();

        bool may_adopt = false;
        m_loader->let_host_adopt_response_body(host_view.page(), [&] { may_adopt = true; });
        Core::EventLoop::current().spin_until([&] { return may_adopt; });
        (void)m_loader->take_result();
    }

    Optional<u64> report_download()
    {
        auto& stub = static_cast<WebContentClientStub&>(m_view.page().client());
        auto lease = transfer_lease();
        return stub.did_start_download(m_view.page().id(), m_view.traversable().id(), m_navigation_id, m_url, "response.bin"sv, s_response_body.length(), HTTP::Cookie::SameSiteContext::same_site(), lease.source_client_id, lease.source_request_id, {}, {}).take_download_id();
    }

private:
    WebView::HeadlessWebView& m_view;
    URL::URL m_url;
    Utf16String m_navigation_id;
    NonnullRefPtr<WebView::NavigationLoader> m_loader;
    RefPtr<Requests::Request> m_fetch;
    bool m_received_response_headers { false };
};

// A client's adoption of a leased response, in the way the process hosting a document adopts its response body.
class TestAdoption {
    AK_MAKE_NONCOPYABLE(TestAdoption);
    AK_MAKE_NONMOVABLE(TestAdoption);

public:
    AK_ALLOC_WITH_KMALLOC;

    TestAdoption(Requests::RequestClient& client, Requests::RequestTransferLeaseKey lease)
        : m_request(client.adopt_request(lease.source_client_id, lease.source_request_id, Requests::RequestClient::TransferLease::Yes).release_nonnull())
    {
        m_request->set_body_delivery_paused(true);
        m_request->set_unbuffered_request_callbacks(
            [this](auto, auto, auto const&, auto) { m_succeeded = true; },
            [](auto) { },
            [](auto) { },
            [this](auto, auto const&, auto) { m_succeeded = false; });
    }

    bool succeeded()
    {
        Core::EventLoop::current().spin_until([&] { return m_succeeded.has_value(); });
        return *m_succeeded;
    }

private:
    NonnullRefPtr<Requests::Request> m_request;
    Optional<bool> m_succeeded;
};

}

// The UI process takes over a navigation's response body only from a RequestServer client it gave the renderer naming
// it. A renderer naming the client of another live renderer, of a worker or of the UI process is terminated, and any
// other client's body is left alone.
TEST_CASE(renderer_response_body_claims)
{
    TestFixture fixture;
    auto& app = fixture.application();
    TestResponseServer server;

    auto victim_view = fixture.create_view();
    auto victim_client_id = victim_view->page().client().request_server_site_bindings().client_id().value();

    auto view = fixture.create_view();
    auto& stub = static_cast<WebContentClientStub&>(view->page().client());
    auto page_id = view->page().id();
    auto navigable_id = view->traversable().id();
    auto own_client_id = view->page().client().request_server_site_bindings().client_id().value();
    VERIFY(own_client_id != victim_client_id);

    // A renderer's own response body is not misbehavior.
    stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(own_client_id, 0));
    stub.did_finish_history_navigation_params_creation(page_id, app.allocate_ui_process_cross_process_id(), navigable_id, result_with_response_body_of(own_client_id, 0));
    EXPECT(view->page().client().is_open());

    // Nor is the body of a client no live renderer holds. The UI process takes none of it, whether the result is
    // discarded or continues a navigation in flight.
    auto new_client = MUST(WebView::connect_new_request_server_client(WebView::Application::default_session(), RequestServer::SiteBinding::Bound));
    auto other_client = make_ref_counted<Requests::RequestClient>(MUST(new_client.handle.create_transport()));
    auto other_request = start_leased_request(*other_client, server.unique_url());
    auto other_result = [&] { return result_with_response_body_of(other_client->request_server_client_id(), other_request->id()); };

    stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, other_result());
    stub.did_finish_history_navigation_params_creation(page_id, app.allocate_ui_process_cross_process_id(), navigable_id, other_result());

    auto navigation_id = start_navigation(*view);
    stub.did_finish_navigation_params_creation(page_id, navigable_id, navigation_id, other_result());
    EXPECT(!view->traversable().has_uncommitted_navigation());
    EXPECT(view->page().client().is_open());

    // Every adoption above has reached RequestServer once the UI process's own client is answered.
    auto other_request_server = WebView::RequestServerManager::the().instance_for_client(other_client->request_server_client_id());
    VERIFY(other_request_server);
    (void)other_request_server->ui_client().send_sync<Messages::RequestServer::GetClientId>();
    EXPECT(other_client->send_sync<Messages::RequestServer::StopRequest>(other_request->id())->success());

    auto expect_rejected_from_new_view = [&](StringView what, Function<void(WebContentClientStub&, Web::PageId, Web::HTML::CrossProcessId)> send) {
        auto view = fixture.create_view();
        expect_rejected(*view, what, [&] {
            send(static_cast<WebContentClientStub&>(view->page().client()), view->page().id(), view->traversable().id());
        });
    };

    // A body fetched by an earlier client of the population worker is still the worker's own, so the UI process stops
    // it along with a discarded result.
    auto earlier_client = connect_client(*view);
    auto later_client = connect_client(*view);
    auto earlier_client_id = earlier_client->request_server_client_id();
    auto earlier_request = start_leased_request(*earlier_client, server.unique_url());
    stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(earlier_client_id, earlier_request->id()));
    EXPECT(view->page().client().is_open());
    auto earlier_request_server = WebView::RequestServerManager::the().instance_for_client(earlier_client_id);
    VERIFY(earlier_request_server);
    (void)earlier_request_server->ui_client().send_sync<Messages::RequestServer::GetClientId>();
    EXPECT(!earlier_client->send_sync<Messages::RequestServer::StopRequest>(earlier_request->id())->success());

    // It is not any other process's own.
    expect_rejected_from_new_view("a navigation's response body fetched by an earlier client of another renderer"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(earlier_client_id, 0));
    });

    // A client that has disconnected is no longer any process's own.
    auto const& bindings = view->page().client().request_server_site_bindings();
    EXPECT(bindings.has_connected_client(earlier_client_id));
    earlier_client->shutdown();
    Core::EventLoop::current().spin_until([&] { return !bindings.has_connected_client(earlier_client_id); });
    auto& victim_stub = static_cast<WebContentClientStub&>(victim_view->page().client());
    victim_stub.did_finish_navigation_params_creation(victim_view->page().id(), victim_view->traversable().id(), "unknown"_utf16, result_with_response_body_of(earlier_client_id, 0));
    EXPECT(victim_view->page().client().is_open());

    expect_rejected_from_new_view("a navigation's response body fetched by another renderer"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(victim_client_id, 0));
    });
    expect_rejected_from_new_view("a history navigation's response body fetched by another renderer"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_history_navigation_params_creation(page_id, app.allocate_ui_process_cross_process_id(), navigable_id, result_with_response_body_of(victim_client_id, 0));
    });
    expect_rejected_from_new_view("a navigation's response body held by the UI process"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(other_request_server->ui_client().request_server_client_id(), 0));
    });

    auto worker_view = fixture.create_view();
    worker_view->load_html("<script>new Worker('data:text/javascript,setInterval(() => {}, 1000)')</script>"sv);
    Optional<int> worker_client_id;
    Core::EventLoop::current().spin_until([&]() {
        WebView::WorkerAgentManager::the().for_each_request_server_site_bindings([&](auto& bindings) {
            worker_client_id = bindings.client_id();
            return worker_client_id.has_value() ? IterationDecision::Break : IterationDecision::Continue;
        });
        return worker_client_id.has_value();
    });
    expect_rejected_from_new_view("a navigation's response body fetched by a worker"sv, [&](auto& stub, auto page_id, auto navigable_id) {
        stub.did_finish_navigation_params_creation(page_id, navigable_id, "unknown"_utf16, result_with_response_body_of(*worker_client_id, 0));
    });
}

// A host adopts a response body through its current client of the site, whichever of its clients fetched the response.
TEST_CASE(host_adopts_response_body_through_its_client_of_the_site)
{
    TestFixture fixture;
    TestResponseServer server;
    auto view = fixture.create_view();

    auto url = server.unique_url();
    auto top_level_site = URL::Site::serialize_for_partitioning(url.origin());
    TestPopulation population { *view, url };

    auto fetching_client = connect_client(*view, top_level_site);
    auto host_client = connect_client(*view, top_level_site);
    population.fetch_response_through(*fetching_client, top_level_site);

    population.hand_response_body_to(*view);
    EXPECT_EQ(population.loader().response_body_adopter_client_id(), host_client->request_server_client_id());

    TestAdoption adoption { *host_client, population.transfer_lease() };
    EXPECT(adoption.succeeded());
}

// A host without a client of the document's top-level site adopts the response body through its client without a site.
TEST_CASE(host_without_a_client_of_the_site_adopts_response_body_through_its_client_without_a_site)
{
    TestFixture fixture;
    auto page = "<!DOCTYPE html>"sv;
    TestResponseServer page_server { "text/html"sv, page, page.length() };
    TestResponseServer server;

    auto source_view = fixture.create_view();
    load_page(*source_view, page_server.unique_url());
    auto& document = source_view->traversable().active_document();
    VERIFY(document.top_level_origin().has_value());
    VERIFY(URL::Site::serialize_for_partitioning(*document.top_level_origin()).has_value());

    auto host_view = fixture.create_view();
    auto fetching_client = connect_client(*host_view);
    auto host_client = connect_client(*host_view);
    EXPECT(!host_view->page().client().request_server_site_bindings().client_id_for(URL::Site::serialize_for_partitioning(*document.top_level_origin())).has_value());

    TestPopulation population { *host_view, server.unique_url() };
    population.fetch_response_through(*fetching_client);
    population.loader().set_document(document, source_view->traversable());
    population.move_response_body_to(*host_view);

    population.hand_response_body_to(*host_view);
    EXPECT_EQ(population.loader().response_body_adopter_client_id(), host_client->request_server_client_id());

    TestAdoption adoption { *host_client, population.transfer_lease() };
    EXPECT(adoption.succeeded());
}

// A process that reports a download of a response body handed to another process is terminated.
TEST_CASE(download_of_response_body_handed_to_another_process_is_rejected)
{
    TestFixture fixture;
    TestResponseServer server;
    auto view = fixture.create_view();
    auto host_view = fixture.create_view();

    auto fetching_client = connect_client(*view);
    TestPopulation population { *view, server.unique_url() };
    population.fetch_response_through(*fetching_client);
    population.hand_response_body_to(*host_view);

    expect_rejected(*view, "a download of a response body handed to another process"sv, [&] {
        (void)population.report_download();
    });
}

// A process reporting a download of a response body no client of it may adopt is refused, which ends the navigation, as
// the process ends its side of it. The process is not at fault while the response body has no other recipient.
TEST_CASE(download_of_response_body_with_no_client_recorded_is_refused)
{
    TestFixture fixture;
    TestResponseServer server;
    auto view = fixture.create_view();
    auto fetching_client = connect_client(*view);

    auto expect_refused = [&](TestPopulation& population) {
        EXPECT(!population.report_download().has_value());
        EXPECT(!population.is_in_flight());
        EXPECT(view->page().client().is_open());
    };

    // The UI process has handed the response body to no process.
    TestPopulation population_before_handing_over { *view, server.unique_url() };
    population_before_handing_over.fetch_response_through(*fetching_client);
    expect_refused(population_before_handing_over);

    // The host has no client that may adopt the response body.
    TestPopulation population_without_adopter { *view, server.unique_url() };
    population_without_adopter.fetch_response_through(*fetching_client, "https://site-without-a-client.example"_utf16);
    population_without_adopter.hand_response_body_to(*view);
    EXPECT(!population_without_adopter.loader().response_body_adopter_client_id().has_value());
    expect_refused(population_without_adopter);

    // The navigation has ended.
    expect_refused(population_without_adopter);
}

// A URL a process reports does not make a navigation the UI process did not admit to a file: URL into one.
TEST_CASE(made_up_file_url_does_not_reach_a_navigation)
{
    TestFixture fixture;
    auto view = fixture.create_view();
    auto& stub = static_cast<WebContentClientStub&>(view->page().client());
    auto client_id = view->page().client().request_server_site_bindings().client_id().value();

    auto navigation_id = start_navigation(*view);
    auto result = result_with_response_body_of(client_id, 0);
    result.redirected_url = URL::Parser::basic_parse("file:///made-up"sv).release_value();
    stub.did_finish_navigation_params_creation(view->page().id(), view->traversable().id(), navigation_id, move(result));

    auto navigation = view->traversable().navigation_with_id(navigation_id);
    VERIFY(navigation);
    EXPECT_NE(navigation->url.scheme(), "file"sv);
}

// A renderer whose connection to RequestServer was replaced keeps the earlier connection until it closes it. The UI
// process takes a download from that connection, whether or not it has adopted the response body.
TEST_CASE(download_from_a_replaced_connection_that_is_still_open)
{
    TestFixture fixture;
    MUST(Core::Environment::set("XDG_DOWNLOAD_DIR"sv, fixture.directory(), Core::Environment::Overwrite::Yes));

    for (bool host_adopted : { true, false }) {
        TestResponseServer server;
        auto view = fixture.create_view();

        auto host_client = connect_client(*view);
        TestPopulation population { *view, server.unique_url() };
        population.fetch_response_through(*host_client);
        population.hand_response_body_to(*view);
        EXPECT_EQ(population.loader().response_body_adopter_client_id(), host_client->request_server_client_id());

        OwnPtr<TestAdoption> adoption;
        if (host_adopted) {
            adoption = make<TestAdoption>(*host_client, population.transfer_lease());
            EXPECT(adoption->succeeded());
        }

        auto later_client = connect_client(*view);

        FinishedDownloadRecorder recorder;
        auto download_id = population.report_download();
        VERIFY(download_id.has_value());
        EXPECT(!population.is_in_flight());

        server.send_rest_of_last_response();
        Core::EventLoop::current().spin_until([&] { return recorder.finished().has_value(); });
        EXPECT_EQ(recorder.finished()->status, WebView::FileDownloader::DownloadStatus::Completed);

        auto file = MUST(Core::File::open(recorder.finished()->destination.string(), Core::File::OpenMode::Read));
        auto contents = MUST(file->read_until_eof());
        EXPECT_EQ(StringView { contents }, s_response_body);
        EXPECT(host_client->is_open());
    }
}
