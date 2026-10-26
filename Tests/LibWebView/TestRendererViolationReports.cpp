/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalNavigation.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

struct Page {
    ByteString headers;
    ByteString body;
};

// Answers each path with its page, leaves a request for /held unanswered while the server lives, and counts the
// requests for /violation-report and /report-barrier.
class TestHttpServer {
public:
    explicit TestHttpServer(HashMap<ByteString, Page> pages)
        : m_pages(move(pages))
    {
        m_socket = MUST(Core::System::socket(AF_INET, SOCK_STREAM, 0));

        int reuse_address = 1;
        MUST(Core::System::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        MUST(Core::System::bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
        MUST(Core::System::listen(m_socket, 16));

        socklen_t address_size = sizeof(address);
        VERIFY(::getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
        m_port = ntohs(address.sin_port);

        m_thread = Threading::Thread::construct("TestHttpServer"sv, [this] -> intptr_t {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~TestHttpServer()
    {
        m_shutting_down.store(true);
        wake_server();
        (void)m_thread->join();
        for (auto client : m_held_clients)
            (void)Core::System::close(client);
        (void)Core::System::close(m_socket);
    }

    URL::URL url(StringView host, StringView path) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://{}:{}{}", host, m_port, path)).release_value();
    }

    size_t violation_reports() const { return m_violation_reports.load(); }
    size_t report_barriers() const { return m_report_barriers.load(); }

private:
    void run()
    {
        while (!m_shutting_down.load()) {
            auto client = ::accept(m_socket, nullptr, nullptr);
            if (client < 0)
                return;
            if (respond(client))
                m_held_clients.append(client);
            else
                (void)Core::System::close(client);
        }
    }

    void wake_server()
    {
        auto socket = Core::System::socket(AF_INET, SOCK_STREAM, 0);
        if (socket.is_error())
            return;

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(m_port);
        (void)::connect(socket.value(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
        (void)Core::System::close(socket.value());
    }

    // Returns whether the request is held, unanswered.
    bool respond(int client)
    {
        StringBuilder request;
        Array<u8, 4096> buffer;
        while (!request.string_view().contains("\r\n\r\n"sv)) {
            auto read = Core::System::read(client, buffer);
            if (read.is_error() || read.value() == 0)
                return false;
            request.append(StringView { buffer.span().trim(read.value()) });
        }

        auto request_line = request.string_view().split_view("\r\n"sv).first().split_view(' ');
        if (request_line.size() < 2)
            return false;
        auto path = request_line[1];
        if (path == "/held"sv)
            return true;
        if (path == "/violation-report"sv)
            m_violation_reports.fetch_add(1);
        if (path == "/report-barrier"sv)
            m_report_barriers.fetch_add(1);

        auto page = m_pages.get(path);
        auto response = page.has_value()
            ? ByteString::formatted("HTTP/1.1 200 OK\r\n{}Content-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", page->headers, page->body.length(), page->body)
            : ByteString { "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"sv };
        (void)write_all(client, response.bytes());
        return false;
    }

    static bool write_all(int fd, ReadonlyBytes bytes)
    {
        while (!bytes.is_empty()) {
            auto written = Core::System::write(fd, bytes);
            if (written.is_error() || written.value() == 0)
                return false;
            bytes = bytes.slice(written.value());
        }
        return true;
    }

    HashMap<ByteString, Page> m_pages;
    Vector<int> m_held_clients;
    int m_socket { -1 };
    u16 m_port { 0 };
    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_shutting_down { false };
    Atomic<size_t> m_violation_reports { 0 };
    Atomic<size_t> m_report_barriers { 0 };
};

bool spin_until_or_timeout(Function<bool()> condition, int timeout_ms)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timeout_ms), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return condition() || timed_out; });
    return condition();
}

void expect(bool condition, StringView what)
{
    if (condition)
        return;
    warnln("FAIL: {}", what);
    VERIFY_NOT_REACHED();
}

// The liar's page holds a child frame whose document has the given environment's id.
void claim_environment(WebView::HeadlessWebView& liar, Web::HTML::EnvironmentId const& environment_id, u64 namespace_id)
{
    auto& stub = static_cast<WebContentClientStub&>(liar.page().client());
    auto& traversable = liar.traversable();

    Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry;
    initial_history_entry.url = URL::about_blank();
    initial_history_entry.document_state.id = { .namespace_id = namespace_id, .local_id = 1 };

    Web::HTML::CrossProcessId frame_id { .namespace_id = namespace_id, .local_id = 2 };
    Web::HTML::CrossProcessId document_id { .namespace_id = namespace_id, .local_id = 3 };
    stub.did_create_child_frame(liar.page().id(), traversable.id(), traversable.active_document().id(), frame_id, {}, move(initial_history_entry), URL::Origin::create_opaque(), environment_id, document_id);
    expect(liar.page().hosted_environment(environment_id).has_value(), "a liar claims the source's environment"sv);
}

}

// A violation of a policy of a navigation's source is reported in the page the UI process found hosting the source
// document when the navigation was requested, whichever other pages claim the source's environment. The navigation's
// population worker is terminated for a report of a directive its kind of navigation rules out, such as form-action
// outside a form submission; a report from another page, or for a navigation the worker is not running, is dropped.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRendererViolationReports-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

    signal(SIGPIPE, SIG_IGN);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    // The source's policy allows its frame's own site only. Each load of the frame and each violation retitles it.
    HashMap<ByteString, Page> pages;
    pages.set("/source"sv, {
                               .headers = "Content-Security-Policy: frame-src http://target.test:*\r\n",
                               .body = R"(<!DOCTYPE html><body><script>
        let loads = 0;
        let violations = 0;
        document.addEventListener("securitypolicyviolation", event => (document.title = `violation ${++violations}: ${event.effectiveDirective}`));
        const frame = document.createElement("iframe");
        frame.name = "target";
        frame.onload = () => (document.title = `loaded ${++loads}`);
        frame.src = `http://target.test:${location.port}/target`;
        document.body.append(frame);
        function navigateTarget(host, path) {
            const link = document.createElement("a");
            link.target = "target";
            link.href = `http://${host}:${location.port}${path}`;
            document.body.append(link);
            link.click();
        }
    </script>)",
                           });
    pages.set("/target"sv, { .headers = {}, .body = "<!DOCTYPE html>target" });

    // A source frame beside its sibling, in a parent of its own origin that keeps the source's document alive once
    // the source navigates away. A violation of the source's policy posts to /violation-report.
    pages.set("/frames"sv, { .headers = {}, .body = R"(<!DOCTYPE html><body><script>
        let sourceLoads = 0;
        let siblingLoads = 0;
        const sourceFrame = document.createElement("iframe");
        sourceFrame.onload = () => (document.title = `source loaded ${++sourceLoads}`);
        sourceFrame.src = "/source-frame";
        const sibling = document.createElement("iframe");
        sibling.name = "sibling";
        sibling.onload = () => (document.title = `sibling loaded ${++siblingLoads}`);
        sibling.src = `http://target.test:${location.port}/target`;
        document.body.append(sourceFrame, sibling);
        function replaceSource() {
            window.replacedSourceDocument = sourceFrame.contentDocument;
            sourceFrame.contentWindow.replacedSourceGlobal = true;
            sourceFrame.onload = () => (document.title = `source replaced, ${"replacedSourceGlobal" in sourceFrame.contentWindow ? "same" : "new"} global`);
            sourceFrame.src = "/source-replacement";
        }
    </script>)" });
    pages.set("/source-frame"sv, {
                                     .headers = "Content-Security-Policy: frame-src http://target.test:*; report-uri /violation-report\r\n",
                                     .body = R"(<!DOCTYPE html><body><script>
        function navigateSibling(host, path) {
            const link = document.createElement("a");
            link.target = "sibling";
            link.href = `http://${host}:${location.port}${path}`;
            document.body.append(link);
            link.click();
        }
    </script>)",
                                 });
    pages.set("/source-replacement"sv, { .headers = {}, .body = "<!DOCTYPE html>replacement" });
    TestHttpServer server { move(pages) };

    Vector<NonnullOwnPtr<WebView::HeadlessWebView>> liars;
    auto create_liar = [&] {
        auto liar = WebView::HeadlessWebView::create(theme, { 800, 600 });
        bool loaded = false;
        liar->on_load_finish = [&](auto const&) { loaded = true; };
        liar->load(server.url("liar.test"sv, "/target"sv));
        expect(spin_until_or_timeout([&] { return loaded; }, 10'000), "a liar's page loads"sv);
        liar->on_load_finish = {};
        liars.append(move(liar));
    };

    // Liars are created both before and after the source.
    create_liar();
    create_liar();
    auto source_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    create_liar();
    create_liar();

    Vector<ByteString> titles;
    source_view->on_title_change = [&](auto const& title) { titles.append(title.to_utf8().to_byte_string()); };
    auto saw_title = [&](StringView title) { return titles.contains_slow(title); };

    source_view->load(server.url("source.test"sv, "/source"sv));
    expect(spin_until_or_timeout([&] { return saw_title("loaded 1"sv); }, 10'000), "the source's frame loads"sv);

    auto& target = *source_view->traversable().children().first();
    auto environment_id = source_view->traversable().active_document().relevant_global_object().relevant_settings_object().id();
    for (size_t i = 0; i < liars.size(); ++i)
        claim_environment(*liars[i], environment_id, 0xfeed0000 + i);

    source_view->run_javascript("navigateTarget('blocked.test', '/blocked')"_string);
    expect(spin_until_or_timeout([&] { return saw_title("violation 1: frame-src"sv) && saw_title("loaded 2"sv); }, 10'000), "the source sees the violation of its frame-src"sv);

    source_view->run_javascript("navigateTarget('target.test', '/held')"_string);
    RefPtr<WebView::CanonicalNavigation> held_navigation;
    expect(spin_until_or_timeout([&] {
        held_navigation = target.uncommitted_navigation();
        return held_navigation && held_navigation->state.has<WebView::CanonicalNavigation::CreatingNavigationParams>();
    },
               10'000),
        "the frame's navigation is fetching"sv);

    // The page running a navigation's population is its worker.
    auto worker = held_navigation->page;
    auto& source_page = source_view->page();
    expect(worker && &worker->client() != &source_page.client(), "another renderer runs the frame's navigation"sv);

    auto report = [&](WebView::WebContentPage& page, WebView::CanonicalNavigable& navigable, Utf16String navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective directive, u64 policy_index = 0) {
        auto& stub = static_cast<WebContentClientStub&>(page.client());
        stub.did_report_navigation_violation(page.id(), navigable.id(), move(navigation_id), policy_index, directive, false);
    };

    report(*worker, target, "not-the-navigation"_utf16, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    expect(worker->client().is_open(), "a report for another navigation is dropped"sv);

    report(source_page, target, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    expect(source_page.client().is_open(), "a report from a page that is not the navigation's worker is dropped"sv);

    // The source reports only for its policy at the given index, and only one governing the directive. Both channels
    // keep their order, so the last report's violation comes after every earlier report's.
    report(*worker, target, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc, 7);
    report(*worker, target, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::ObjectSrc);
    report(*worker, target, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    report(*worker, target, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    expect(spin_until_or_timeout([&] { return saw_title("violation 3: frame-src"sv); }, 10'000), "the source reports the reports for its governing policy"sv);
    expect(!any_of(titles, [](auto const& title) { return title.contains("object-src"sv); }), "the source drops a report its policy does not govern"sv);
    expect(saw_title("violation 2: frame-src"sv), "the source drops a report for a policy it does not have"sv);

    report(*worker, target, held_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FormAction);
    expect(!worker->client().is_open(), "the worker is terminated for a directive the navigation cannot violate"sv);

    // A source replaced while its navigation runs is no longer reported in, though its document is still alive.
    auto frames_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    Vector<ByteString> frames_titles;
    frames_view->on_title_change = [&](auto const& title) { frames_titles.append(title.to_utf8().to_byte_string()); };
    auto frames_saw_title = [&](StringView title) { return frames_titles.contains_slow(title); };

    frames_view->load(server.url("source.test"sv, "/frames"sv));
    expect(spin_until_or_timeout([&] { return frames_saw_title("source loaded 1"sv) && frames_saw_title("sibling loaded 1"sv); }, 10'000), "the source and its sibling load"sv);

    frames_view->run_javascript("sourceFrame.contentWindow.navigateSibling('blocked.test', '/blocked')"_string);
    expect(spin_until_or_timeout([&] { return server.violation_reports() == 1; }, 10'000), "the source posts its violation's report"sv);

    frames_view->run_javascript("sourceFrame.contentWindow.navigateSibling('target.test', '/held')"_string);
    RefPtr<WebView::CanonicalNavigation> sibling_navigation;
    WebView::CanonicalNavigable* sibling = nullptr;
    expect(spin_until_or_timeout([&] {
        for (auto& child : frames_view->traversable().children()) {
            auto navigation = child->uncommitted_navigation();
            if (navigation && navigation->state.has<WebView::CanonicalNavigation::CreatingNavigationParams>()) {
                sibling_navigation = navigation;
                sibling = child.ptr();
                return true;
            }
        }
        return false;
    },
               10'000),
        "the sibling's navigation is fetching"sv);
    auto sibling_worker = sibling_navigation->page;
    expect(sibling_worker && &sibling_worker->client() != &frames_view->page().client(), "another renderer runs the sibling's navigation"sv);

    frames_view->run_javascript("replaceSource()"_string);
    expect(spin_until_or_timeout([&] { return frames_saw_title("source replaced, new global"sv); }, 10'000), "the source is replaced by a document with an environment of its own"sv);

    // Listeners on a replaced document do not run, but a report its policy sent would reach the report endpoint soon
    // after a request the parent makes in a later task.
    report(*sibling_worker, *sibling, sibling_navigation->navigation_id, Web::ContentSecurityPolicy::RemoteViolationDirective::FrameSrc);
    frames_view->run_javascript("setTimeout(() => fetch('/report-barrier', { method: 'POST', keepalive: true }))"_string);
    expect(spin_until_or_timeout([&] { return server.report_barriers() == 1; }, 10'000), "the parent makes a request in a later task"sv);
    expect(!spin_until_or_timeout([&] { return server.violation_reports() > 1; }, 500), "a report for a replaced source is dropped"sv);
    expect(sibling_worker->client().is_open(), "a report for a replaced source is no fault of its worker"sv);

    outln("PASS: violation reports reach the source's page, and a report of a directive the navigation rules out terminates its renderer");
    return 0;
}
