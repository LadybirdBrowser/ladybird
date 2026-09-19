/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        browser_options.allow_popups = WebView::AllowPopups::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

// Connects a new RequestServer client the way the UI process does for its helpers, and asks RequestServer which ID
// it gave the client. The clients stay connected until the test ends, as RequestServer reuses the IDs of closed ones.
Vector<NonnullRefPtr<Requests::RequestClient>> s_clients;

int client_id_of(Requests::RequestClient& client)
{
    return client.send_sync<Messages::RequestServer::GetClientId>()->client_id();
}

int connect_request_server_client(WebView::BrowsingSession& session)
{
    auto handle = MUST(WebView::connect_new_request_server_client(session));
    auto client = make_ref_counted<Requests::RequestClient>(MUST(handle.create_transport()));
    auto client_id = client->send_sync<Messages::RequestServer::GetClientId>()->client_id();
    s_clients.append(move(client));
    return client_id;
}

}

// RequestServer asks the UI process for the cookies of a request by the ID of the client that made it. The UI process
// uses the browsing session that it created the client for, so a client of a private session that has since been
// restarted never gets the new session's cookies.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRequestServerClientSessions-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto normal_client = connect_request_server_client(WebView::Application::default_session());
    VERIFY(app->session_for_request_server_client(normal_client).ptr() == &WebView::Application::default_session());

    // The views of the first private session keep it alive after the user restarts private browsing.
    auto first_private_session = WebView::Application::session_for_new_view(WebView::IsPrivate::Yes);
    auto first_private_client = connect_request_server_client(*first_private_session);
    auto first_ui_private_client = client_id_of(WebView::Application::request_server_client(WebView::IsPrivate::Yes));
    VERIFY(app->session_for_request_server_client(first_ui_private_client) == first_private_session);

    app->reset_private_browsing_session();
    auto second_private_session = WebView::Application::session_for_new_view(WebView::IsPrivate::Yes);
    auto second_private_client = connect_request_server_client(*second_private_session);

    // The UI process's own private client is replaced along with the session, so its requests use the new cookies.
    auto second_ui_private_client = client_id_of(WebView::Application::request_server_client(WebView::IsPrivate::Yes));
    VERIFY(app->session_for_request_server_client(second_ui_private_client) == second_private_session);

    VERIFY(first_private_session.ptr() != second_private_session.ptr());
    VERIFY(app->session_for_request_server_client(first_private_client) == first_private_session);
    VERIFY(app->session_for_request_server_client(second_private_client) == second_private_session);

    // A client ID that was never handed out belongs to no session.
    VERIFY(!app->session_for_request_server_client(-1));

    // When RequestServer restarts, every WebContent process gets a replacement client. The one for a view of the old
    // private session must keep using that session's cookies.
    auto theme = TRY(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    app->reset_private_browsing_session();
    auto old_private_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    auto& old_private_session = old_private_view->client().session();
    app->reset_private_browsing_session();
    auto new_private_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    auto& new_private_session = new_private_view->client().session();
    VERIFY(&old_private_session != &new_private_session);

    // Pages that a private view opens are private too, whether they share its process or get their own.
    Vector<bool> popups_are_private;
    auto open_popup = move(new_private_view->on_new_web_view);
    new_private_view->on_new_web_view = [&](auto activate_tab, auto hints, Optional<Web::PageId> page_index) {
        auto handle = open_popup(activate_tab, hints, page_index);
        popups_are_private.append(WebView::ViewImplementation::find_view_by_handle(handle)->is_private() == WebView::IsPrivate::Yes);
        return handle;
    };
    new_private_view->run_javascript("window.open('about:blank'); window.open('about:blank', '_blank', 'noopener');"_string);
    Core::EventLoop::current().spin_until([&] { return popups_are_private.size() == 2; });
    VERIFY(popups_are_private[0] && popups_are_private[1]);

    // Holding on to the old control client keeps its address from being reused by the replacement.
    NonnullRefPtr<Requests::RequestControlClient> old_control_client = WebView::Application::request_server_control_client();
    WebView::Application::process_manager().for_each_process([](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::RequestServer)
            MUST(Core::System::kill(process.pid(), SIGKILL));
    });
    Core::EventLoop::current().spin_until([&] {
        return &WebView::Application::request_server_control_client() != old_control_client.ptr();
    });

    VERIFY(!app->request_server_client_ids_for_testing(old_private_session).is_empty());
    VERIFY(!app->request_server_client_ids_for_testing(new_private_session).is_empty());

    // RequestServer hands out a client ID only once, so the UI process forgets a client when it disconnects.
    auto disconnecting_client = connect_request_server_client(WebView::Application::default_session());
    VERIFY(app->session_for_request_server_client(disconnecting_client));
    s_clients.clear();
    Core::EventLoop::current().spin_until([&] { return !app->session_for_request_server_client(disconnecting_client); });
    outln("PASS: RequestServer clients use the cookies of the browsing session they were created for");
    return 0;
}
