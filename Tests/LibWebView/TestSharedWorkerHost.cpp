/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/IPv4Address.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WorkerAgentManager.h>

namespace {

// The page shows what the worker says in its title, over and over, so a test sees the worker's state at any time.
constexpr auto page = R"html(<!DOCTYPE html><script>
const parameters = new URLSearchParams(location.search);
const worker = new SharedWorker(parameters.get("script") ?? "/worker.js", {
    name: parameters.get("name"),
    extendedLifetime: parameters.has("extended"),
});
const pings = setInterval(() => worker.port.postMessage("ping"), 5);
worker.onerror = () => {
    clearInterval(pings);
    document.title = "error";
};
worker.port.onmessage = event => {
    if (document.title !== "error")
        document.title = event.data;
};
</script>)html"sv;

constexpr auto worker_script = R"js(
const id = Math.random().toString(36).slice(2);
let owners = 0;
let pings = 0;
const lastPings = new Map();
onconnect = event => {
    ++owners;
    const port = event.ports[0];
    port.onmessage = () => {
        const now = performance.now();
        lastPings.set(port, now);
        const pingingOwners = [...lastPings.values()].filter(time => now - time < 100).length;
        port.postMessage(`${id} ${owners} ${++pings} ${pingingOwners}`);
    };
    port.start();
};
)js"sv;

constexpr auto cross_origin_isolation_headers = "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n"sv;

class PageServer {
public:
    PageServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] { read_from(connection); };
        };
    }

    URL::URL url(StringView host, StringView path_and_query) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://{}:{}{}", host, *m_server->local_port(), path_and_query)).release_value();
    }

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer data {};
    };

    void read_from(Connection& connection)
    {
        auto buffer = MUST(ByteBuffer::create_uninitialized(16 * KiB));
        auto received = connection.socket->read_some(buffer);
        if (received.is_error() || received.value().is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }
        connection.data.append(received.value());

        auto data = StringView { connection.data.bytes() };
        if (!data.contains("\r\n\r\n"sv))
            return;

        auto path = data.split_view("\r\n"sv)[0].split_view(' ')[1];
        auto is_script = path.starts_with("/worker"sv) || path.starts_with("/isolated-worker"sv);
        auto is_isolated = path.starts_with("/isolated"sv);
        auto body = is_script ? worker_script : page;
        auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n{}\r\n{}",
            is_script ? "text/javascript"sv : "text/html"sv, body.length(), is_isolated ? cross_origin_isolation_headers : ""sv, body);
        MUST(connection.socket->write_until_depleted(response.bytes()));
        connection.socket->on_ready_to_read = nullptr;
        connection.data.clear();
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
};

struct WorkerStatus {
    ByteString id;
    size_t owners { 0 };
    size_t pings { 0 };
    size_t pinging_owners { 0 };
};

class Browser {
public:
    using Tab = NonnullOwnPtr<WebView::HeadlessWebView>;

    Browser()
        : m_theme(MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string())))
    {
        auto& process_manager = WebView::Application::process_manager();
        process_manager.on_process_exited = [this, previous = move(process_manager.on_process_exited)](WebView::Process&& process, Optional<int> exit_status) {
            m_exit_statuses.set(process.pid(), exit_status);
            previous(move(process), exit_status);
        };
    }

    PageServer const& server() const { return m_server; }

    Tab open(URL::URL const& url, WebView::IsPrivate is_private = WebView::IsPrivate::No)
    {
        auto tab = WebView::HeadlessWebView::create(m_theme, { 800, 600 }, is_private);
        tab->load(url);
        (void)worker_status_of(*tab);
        return tab;
    }

    void close(Tab tab)
    {
        bool closed = false;
        tab->on_close = [&] { closed = true; };
        tab->request_close();
        Core::EventLoop::current().spin_until([&] { return closed; });
    }

    WorkerStatus worker_status_of(WebView::HeadlessWebView const& tab)
    {
        Optional<WorkerStatus> status;
        Core::EventLoop::current().spin_until([&] {
            auto parts = tab.title().to_byte_string().split(' ');
            if (parts.size() != 4)
                return false;
            status = WorkerStatus { parts[0], parts[1].to_number<size_t>().value(), parts[2].to_number<size_t>().value(), parts[3].to_number<size_t>().value() };
            return true;
        });
        return status.release_value();
    }

    // The worker answers a tab after whatever happened to its process.
    void expect_worker_answers(WebView::HeadlessWebView const& tab)
    {
        auto pings = worker_status_of(tab).pings;
        Core::EventLoop::current().spin_until([&] { return worker_status_of(tab).pings > pings; });
    }

    void wait_for_exit(pid_t pid)
    {
        Core::EventLoop::current().spin_until([&] { return m_exit_statuses.contains(pid); });
    }

    void expect_clean_exit(pid_t pid)
    {
        wait_for_exit(pid);
        auto exit_status = m_exit_statuses.get(pid).value();
        EXPECT(exit_status.has_value() && WIFEXITED(*exit_status) && WEXITSTATUS(*exit_status) == 0);
    }

    void wait_for_no_workers()
    {
        Core::EventLoop::current().spin_until([] { return WebView::WorkerAgentManager::the().client_count() == 0; });
    }

    bool has_exited(pid_t pid) const { return m_exit_statuses.contains(pid); }

private:
    Core::AnonymousBuffer m_theme;
    PageServer m_server;
    HashMap<pid_t, Optional<int>> m_exit_statuses;
};

bool hosts_shared_worker(WebView::HeadlessWebView const& tab)
{
    return WebView::WorkerAgentManager::the().hosts_shared_worker(tab.page().client());
}

size_t worker_count()
{
    return WebView::WorkerAgentManager::the().client_count();
}

}

static void two_tabs_share_one_worker_that_outlives_the_tab_hosting_it(Browser& browser)
{
    auto url = browser.server().url("127.0.0.1"sv, "/page?name=shared"sv);
    auto host_tab = browser.open(url);
    auto other_tab = browser.open(url);
    auto host_pid = host_tab->web_content_pid();
    auto other_pid = other_tab->web_content_pid();

    EXPECT_NE(host_pid, other_pid);
    EXPECT_EQ(worker_count(), 1u);
    EXPECT(hosts_shared_worker(*host_tab));
    EXPECT(!hosts_shared_worker(*other_tab));
    EXPECT_EQ(browser.worker_status_of(*host_tab).id, browser.worker_status_of(*other_tab).id);
    EXPECT_EQ(browser.worker_status_of(*other_tab).owners, 2u);

    browser.close(move(host_tab));
    browser.expect_worker_answers(*other_tab);
    EXPECT(!browser.has_exited(host_pid));
    EXPECT_EQ(worker_count(), 1u);

    // That took the last owner, and with it the only thing the host still had to do.
    browser.close(move(other_tab));
    browser.expect_clean_exit(host_pid);
    browser.expect_clean_exit(other_pid);
    EXPECT_EQ(worker_count(), 0u);
}

enum class AskToClose {
    No,
    Yes,
};

static void tab_hosting_the_worker_goes_without_waiting_for_its_process(Browser& browser, AskToClose ask_to_close)
{
    auto url = browser.server().url("127.0.0.1"sv, ask_to_close == AskToClose::Yes ? "/page?name=asked-close"sv : "/page?name=forced-close"sv);
    auto host_tab = browser.open(url);
    auto other_tab = browser.open(url);
    auto host_pid = host_tab->web_content_pid();
    auto other_pid = other_tab->web_content_pid();

    Core::EventLoop::current().spin_until([&] { return browser.worker_status_of(*other_tab).pinging_owners == 2; });

    // The process goes on for the worker, but the document that was in the tab must not go on with it.
    if (ask_to_close == AskToClose::Yes)
        host_tab->request_close();
    {
        auto closed_tab = move(host_tab);
    }
    Core::EventLoop::current().spin_until([&] { return browser.worker_status_of(*other_tab).pinging_owners == 1; });
    browser.expect_worker_answers(*other_tab);
    EXPECT(!browser.has_exited(host_pid));

    browser.close(move(other_tab));
    browser.expect_clean_exit(host_pid);
    browser.expect_clean_exit(other_pid);
}

static void owner_in_the_host_navigates_away(Browser& browser)
{
    auto url = browser.server().url("127.0.0.1"sv, "/page?name=navigation"sv);
    auto host_tab = browser.open(url);
    auto other_tab = browser.open(url);
    auto host_pid = host_tab->web_content_pid();

    auto id = browser.worker_status_of(*other_tab).id;
    host_tab->load(browser.server().url("localhost"sv, "/page?name=navigation"sv));
    Core::EventLoop::current().spin_until([&] { return host_tab->web_content_pid() != host_pid; });
    Core::EventLoop::current().spin_until([&] { return browser.worker_status_of(*host_tab).id != id; });

    browser.expect_worker_answers(*other_tab);
    EXPECT_EQ(browser.worker_status_of(*other_tab).id, id);
    EXPECT(!browser.has_exited(host_pid));

    browser.close(move(other_tab));
    browser.expect_clean_exit(host_pid);
    browser.close(move(host_tab));
    browser.wait_for_no_workers();
}

static void extended_lifetime_outlives_the_last_tab(Browser& browser)
{
    auto url = browser.server().url("127.0.0.1"sv, "/page?name=extended&extended"sv);
    auto first_tab = browser.open(url);
    auto host_pid = first_tab->web_content_pid();
    auto id = browser.worker_status_of(*first_tab).id;
    browser.close(move(first_tab));
    EXPECT_EQ(worker_count(), 1u);
    EXPECT(!browser.has_exited(host_pid));

    // A new owner in another process finds the worker where its last owner left it.
    auto second_tab = browser.open(url);
    EXPECT_NE(second_tab->web_content_pid(), host_pid);
    EXPECT_EQ(browser.worker_status_of(*second_tab).id, id);

    // NB: The last owner leaves with a short timeout, so the test sees the worker end without waiting for 30 seconds.
    auto timeout = AK::Duration::from_milliseconds(50);
    WebView::WorkerAgentManager::the().set_extended_lifetime_shared_worker_timeout_for_testing(timeout);
    auto start = MonotonicTime::now();
    browser.close(move(second_tab));
    browser.expect_clean_exit(host_pid);
    EXPECT((MonotonicTime::now() - start) >= timeout);
    EXPECT_EQ(worker_count(), 0u);
}

static void extended_lifetime_ends_with_the_private_session(Browser& browser)
{
    auto url = browser.server().url("127.0.0.1"sv, "/page?name=private-extended&extended"sv);
    auto tab = browser.open(url, WebView::IsPrivate::Yes);
    auto host_pid = tab->web_content_pid();
    browser.close(move(tab));
    EXPECT_EQ(worker_count(), 1u);

    WebView::Application::the().reset_private_browsing_session();
    browser.expect_clean_exit(host_pid);
    EXPECT_EQ(worker_count(), 0u);
}

static void crash_of_the_host_reaches_every_owner(Browser& browser)
{
    auto url = browser.server().url("127.0.0.1"sv, "/page?name=crash"sv);
    auto host_tab = browser.open(url);
    auto other_tab = browser.open(url);
    auto host_pid = host_tab->web_content_pid();

    host_tab->on_web_content_crashed = [](auto) { };
    host_tab->debug_request("crash-current-page"sv);
    Core::EventLoop::current().spin_until([&] { return other_tab->title() == "error"sv; });
    browser.wait_for_exit(host_pid);
    browser.wait_for_no_workers();

    browser.close(move(other_tab));
}

static void same_url_stays_apart_in_private_and_normal_sessions(Browser& browser)
{
    auto url = browser.server().url("127.0.0.1"sv, "/page?name=sessions"sv);
    auto normal_tab = browser.open(url);
    auto private_tab = browser.open(url, WebView::IsPrivate::Yes);

    EXPECT_NE(normal_tab->web_content_pid(), private_tab->web_content_pid());
    EXPECT_NE(browser.worker_status_of(*normal_tab).id, browser.worker_status_of(*private_tab).id);
    EXPECT_EQ(worker_count(), 2u);
    EXPECT(hosts_shared_worker(*normal_tab));
    EXPECT(hosts_shared_worker(*private_tab));

    browser.close(move(normal_tab));
    browser.close(move(private_tab));
    browser.wait_for_no_workers();
}

static void cross_site_pages_never_share_a_process(Browser& browser)
{
    auto first_tab = browser.open(browser.server().url("127.0.0.1"sv, "/page?name=sites"sv));
    auto second_tab = browser.open(browser.server().url("localhost"sv, "/page?name=sites"sv));

    EXPECT_NE(first_tab->web_content_pid(), second_tab->web_content_pid());
    EXPECT_NE(browser.worker_status_of(*first_tab).id, browser.worker_status_of(*second_tab).id);
    EXPECT(hosts_shared_worker(*first_tab));
    EXPECT(hosts_shared_worker(*second_tab));

    browser.close(move(first_tab));
    browser.close(move(second_tab));
    browser.wait_for_no_workers();
}

static void cross_origin_isolated_owner_gets_a_process_without_a_page_for_its_worker(Browser& browser)
{
    auto tab = browser.open(browser.server().url("127.0.0.1"sv, "/isolated-page?name=isolated&script=/isolated-worker.js"sv));
    EXPECT_EQ(worker_count(), 1u);
    EXPECT(!hosts_shared_worker(*tab));

    Optional<pid_t> host_pid;
    WebView::WebContentClient::for_each_client([&](WebView::WebContentClient& client) {
        if (WebView::WorkerAgentManager::the().hosts_shared_worker(client))
            host_pid = client.pid();
        return IterationDecision::Continue;
    });
    EXPECT(host_pid.has_value());
    EXPECT_NE(*host_pid, tab->web_content_pid());

    browser.close(move(tab));
    browser.expect_clean_exit(*host_pid);
}

TEST_CASE(shared_worker_host)
{
    Test::HeadlessTestEnvironment environment { "TestSharedWorkerHost"sv, 120 };
    auto app = TRY_OR_FAIL(environment.create_application());
    Browser browser;

    two_tabs_share_one_worker_that_outlives_the_tab_hosting_it(browser);
    tab_hosting_the_worker_goes_without_waiting_for_its_process(browser, AskToClose::No);
    tab_hosting_the_worker_goes_without_waiting_for_its_process(browser, AskToClose::Yes);
    owner_in_the_host_navigates_away(browser);
    crash_of_the_host_reaches_every_owner(browser);
    same_url_stays_apart_in_private_and_normal_sessions(browser);
    cross_site_pages_never_share_a_process(browser);
    cross_origin_isolated_owner_gets_a_process_without_a_page_for_its_worker(browser);
    extended_lifetime_ends_with_the_private_session(browser);
    extended_lifetime_outlives_the_last_tab(browser);
}
