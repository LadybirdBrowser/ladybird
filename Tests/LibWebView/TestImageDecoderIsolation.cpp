/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Base64.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGfx/SystemTheme.h>
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/ImageDecoderManager.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <signal.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }
};

HashTable<pid_t> processes_of_type(WebView::ProcessType type)
{
    HashTable<pid_t> pids;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == type)
            pids.set(process.pid());
    });
    return pids;
}

}

static constexpr auto page_with_image = "<!DOCTYPE html><img src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\">"sv;

// An ImageDecoder parses untrusted images, so it is launched only for a process that has some to decode. A process with no
// site has one of its own, and never sees another's images. A decoder that crashes is replaced for its renderer alone, once
// the renderer next has an image to decode.

TEST_CASE(image_decoder_isolation)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecoderIsolation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto first_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    size_t first_view_loads = 0;
    size_t second_view_loads = 0;
    first_view->on_load_finish = [&](auto const&) { ++first_view_loads; };
    second_view->on_load_finish = [&](auto const&) { ++second_view_loads; };
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 1 && second_view_loads >= 1; });

    // Nothing has had an image to decode yet.
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).is_empty());

    // The load event waits for the image, so its decoder has been launched by the time the load finishes.
    first_view->load_html(page_with_image);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 2; });
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).size() == 1);

    second_view->load_html(page_with_image);
    Core::EventLoop::current().spin_until([&] { return second_view_loads >= 2; });
    auto decoders = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders.size() == 2);

    auto crashed_decoder = *decoders.begin();
    TRY_OR_FAIL(Core::System::kill(crashed_decoder, SIGKILL));
    Core::EventLoop::current().spin_until([&] {
        return !processes_of_type(WebView::ProcessType::ImageDecoder).contains(crashed_decoder);
    });

    // Both renderers decode again, and only the one that lost its decoder gets a new one.
    first_view->load_html(page_with_image);
    second_view->load_html(page_with_image);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 3 && second_view_loads >= 3; });

    auto decoders_after_crash = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders_after_crash.size() == 2);
    VERIFY(!decoders_after_crash.contains(crashed_decoder));
    for (auto decoder : decoders) {
        if (decoder != crashed_decoder)
            VERIFY(decoders_after_crash.contains(decoder));
    }

    outln("PASS: a renderer with no site gets an image decoder of its own when it needs one, which is replaced alone when it crashes");
}

// The renderers of one site share its decoder, however many of them there are.

TEST_CASE(renderers_of_a_site_share_an_image_decoder)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecoderSharing"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto page_path = LexicalPath::join(environment.directory(), "image.html"sv).string();
    auto page = TRY_OR_FAIL(Core::File::open(page_path, Core::File::OpenMode::Write));
    TRY_OR_FAIL(page->write_until_depleted(page_with_image.bytes()));
    page->close();
    auto page_url = URL::create_with_file_scheme(page_path).value();

    auto first_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    size_t first_view_loads = 0;
    size_t second_view_loads = 0;
    first_view->on_load_finish = [&](auto const&) { ++first_view_loads; };
    second_view->on_load_finish = [&](auto const&) { ++second_view_loads; };
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 1 && second_view_loads >= 1; });

    first_view->load(page_url);
    second_view->load(page_url);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 2 && second_view_loads >= 2; });

    VERIFY(processes_of_type(WebView::ProcessType::WebContent).size() >= 2);
    auto decoders = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders.size() == 1);

    // A renderer that comes to host a second site leaves the decoder, which stays with the renderer that has only the one.
    first_view->page().client().did_host_document_of_site("https://other.example"_utf16);
    first_view->load(page_url);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 3; });

    auto decoders_after_second_site = processes_of_type(WebView::ProcessType::ImageDecoder);
    VERIFY(decoders_after_second_site.size() == 2);
    VERIFY(decoders_after_second_site.contains(*decoders.begin()));

    outln("PASS: the renderers of a site share one image decoder, which a renderer leaves when it hosts a second site");
}

// The PDF viewer has an origin of its own, but it only shows what the site of its process gave it. A renderer that hosts it
// has not come to host a second site, so it stays with the decoder of its site.

TEST_CASE(pdf_viewer_does_not_make_a_renderer_leave_the_image_decoder_of_its_site)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecoderPdfViewer"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto write_file = [&](StringView name, StringView contents) {
        auto path = LexicalPath::join(environment.directory(), name).string();
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(contents.bytes()));
        file->close();
        return URL::create_with_file_scheme(path).value();
    };
    write_file("document.pdf"sv, "%PDF-1.0\n"sv);
    auto image_page_url = write_file("image.html"sv, page_with_image);
    auto pdf_page_url = write_file("pdf.html"sv, R"html(<!DOCTYPE html>
<iframe src="document.pdf"></iframe>
<script>
    window.onload = () => {
        const image = new Image();
        image.onload = () => { document.title = "decoded"; };
        image.src = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    };
</script>)html"sv);

    auto first_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    size_t first_view_loads = 0;
    size_t second_view_loads = 0;
    bool second_view_decoded_image = false;
    first_view->on_load_finish = [&](auto const&) { ++first_view_loads; };
    second_view->on_load_finish = [&](auto const&) { ++second_view_loads; };
    second_view->on_title_change = [&](auto const& title) { second_view_decoded_image = title == "decoded"sv; };
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 1 && second_view_loads >= 1; });

    first_view->load(image_page_url);
    Core::EventLoop::current().spin_until([&] { return first_view_loads >= 2; });
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).size() == 1);

    second_view->load(pdf_page_url);
    Core::EventLoop::current().spin_until([&] { return second_view_decoded_image; });
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).size() == 1);

    outln("PASS: a renderer that hosts the PDF viewer stays with the image decoder of its site");
}

// Who shares a decoder decides whose images a malicious image can reach, so these are the rules that keep them apart. The
// child IDs stand for processes that do not exist, which the manager has no reason to care about.

TEST_CASE(image_decoder_sharing_rules)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecoderSharingRules"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto& manager = WebView::ImageDecoderManager::the();
    auto site_a = "https://a.example"_utf16;
    auto site_b = "https://b.example"_utf16;

    Vector<IPC::TransportHandle> handles;
    RefPtr<WebView::BrowsingSession> ordinary_session = WebView::Application::session_for_new_view(WebView::IsPrivate::No);
    RefPtr<WebView::BrowsingSession> private_session = WebView::Application::session_for_new_view(WebView::IsPrivate::Yes);
    auto connect = [&](u64 child_id, RefPtr<WebView::BrowsingSession> const& session, Optional<Utf16String> site) {
        handles.append(MUST(manager.connect_new_client(child_id, session, move(site))));
    };
    auto decoder_count = [] { return processes_of_type(WebView::ProcessType::ImageDecoder).size(); };
    auto decoder_of_site = [](StringView site) {
        pid_t pid = 0;
        WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
            if (process.type() == WebView::ProcessType::ImageDecoder && process.site_url().has_value() && process.site_url()->serialize().contains(site))
                pid = process.pid();
        });
        return pid;
    };

    // The processes of a site share a decoder, and those of another site do not.
    connect(1'000'001, ordinary_session, site_a);
    connect(1'000'002, ordinary_session, site_a);
    VERIFY(decoder_count() == 1);
    connect(1'000'003, ordinary_session, site_b);
    VERIFY(decoder_count() == 2);

    // Private browsing shares nothing with the same site outside it.
    connect(1'000'004, private_session, site_a);
    VERIFY(decoder_count() == 3);

    // A process with no site shares with nobody, and asking again launches nothing more.
    connect(1'000'005, ordinary_session, {});
    VERIFY(decoder_count() == 4);
    connect(1'000'005, ordinary_session, {});
    VERIFY(decoder_count() == 4);
    connect(1'000'006, ordinary_session, {});
    VERIFY(decoder_count() == 5);

    // Without site isolation a process may host anything, so its claim to a site does not make it safe to share with.
    auto mode = WebView::site_isolation_mode();
    WebView::set_site_isolation_mode(WebView::SiteIsolationMode::TopLevel);
    connect(1'000'007, ordinary_session, site_a);
    connect(1'000'008, ordinary_session, site_a);
    WebView::set_site_isolation_mode(mode);
    VERIFY(decoder_count() == 7);

    // A session that has ended shares nothing with the next, nor does a worker that outlived its session.
    private_session = nullptr;
    connect(1'000'009, WebView::Application::session_for_new_view(WebView::IsPrivate::Yes), site_a);
    VERIFY(decoder_count() == 8);
    connect(1'000'010, nullptr, site_a);
    VERIFY(decoder_count() == 9);

    // Only a process served by the decoder of a site has one to leave.
    VERIFY(manager.remove_client_from_decoder_of_site(1'000'002));
    VERIFY(!manager.remove_client_from_decoder_of_site(1'000'002));
    VERIFY(!manager.remove_client_from_decoder_of_site(1'000'005));

    // A site that loses its decoder gets a new one, and no other site notices.
    auto decoder_of_a = decoder_of_site("a.example"sv);
    auto crashed_decoder = decoder_of_site("b.example"sv);
    VERIFY(crashed_decoder != 0);
    TRY_OR_FAIL(Core::System::kill(crashed_decoder, SIGKILL));
    Core::EventLoop::current().spin_until([&] {
        return !processes_of_type(WebView::ProcessType::ImageDecoder).contains(crashed_decoder);
    });
    connect(1'000'003, ordinary_session, site_b);
    VERIFY(decoder_count() == 9);
    VERIFY(decoder_of_site("b.example"sv) != crashed_decoder);
    VERIFY(processes_of_type(WebView::ProcessType::ImageDecoder).contains(decoder_of_a));

    // A decoder ends with the last process it serves, and not before.
    handles.clear();
    manager.process_did_exit(1'000'003);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 8; });
    manager.process_did_exit(1'000'005);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 7; });
    for (u64 child_id = 1'000'001; child_id <= 1'000'010; ++child_id)
        manager.process_did_exit(child_id);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });

    outln("PASS: image decoders are shared within a site and nowhere else, and end with the last process they serve");
}

// A process lets go of its decoder once it has nothing left to decode, and the decoder ends once no process uses it.
TEST_CASE(image_decoders_end_once_released)
{
    Test::HeadlessTestEnvironment environment { "TestImageDecodersEndOnceReleased"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto& manager = WebView::ImageDecoderManager::the();
    RefPtr<WebView::BrowsingSession> session = WebView::Application::session_for_new_view(WebView::IsPrivate::No);
    auto site = "https://c.example"_utf16;
    auto decoder_count = [] { return processes_of_type(WebView::ProcessType::ImageDecoder).size(); };

    // The decoder of a site stays for as long as one of its processes uses it.
    Optional<IPC::TransportHandle> first = MUST(manager.connect_new_client(2'000'001, session, site));
    Optional<IPC::TransportHandle> second = MUST(manager.connect_new_client(2'000'002, session, site));
    VERIFY(decoder_count() == 1);
    first.clear();
    manager.process_did_release_decoder(2'000'001);
    VERIFY(decoder_count() == 1);
    second.clear();
    manager.process_did_release_decoder(2'000'002);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });

    // Each worker on a thread of a process has a client of its own, and the process keeps the decoder until all let go.
    Optional<IPC::TransportHandle> page_client = MUST(manager.connect_new_client(2'000'004, session, site));
    Optional<IPC::TransportHandle> worker_client = MUST(manager.connect_new_client(2'000'004, session, site));
    VERIFY(decoder_count() == 1);
    worker_client.clear();
    manager.process_did_release_decoder(2'000'004);
    Optional<IPC::TransportHandle> client_of_same_site = MUST(manager.connect_new_client(2'000'005, session, site));
    VERIFY(decoder_count() == 1);
    page_client.clear();
    client_of_same_site.clear();
    manager.process_did_release_decoder(2'000'004);
    manager.process_did_release_decoder(2'000'005);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });

    // A worker that still holds a client when its process leaves the decoder has no say over the next one.
    Optional<IPC::TransportHandle> client_of_old_decoder = MUST(manager.connect_new_client(2'000'006, session, site));
    auto generation_of_old_decoder = manager.client_generation(2'000'006);
    VERIFY(manager.remove_client_from_decoder_of_site(2'000'006));
    client_of_old_decoder.clear();
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });
    Optional<IPC::TransportHandle> client_of_new_decoder = MUST(manager.connect_new_client(2'000'006, session, site));
    manager.process_did_release_decoder(2'000'006, generation_of_old_decoder);
    VERIFY(decoder_count() == 1);
    client_of_new_decoder.clear();
    manager.process_did_release_decoder(2'000'006, manager.client_generation(2'000'006));
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });

    // A client that was lost without a release counts as one, once the same renderer or worker connects again.
    Optional<u64> client_generation;
    Optional<IPC::TransportHandle> lost_client = MUST(manager.connect_new_client(2'000'007, session, site));
    manager.note_connected_client(2'000'007, client_generation);
    Optional<IPC::TransportHandle> next_client = MUST(manager.connect_new_client(2'000'007, session, site));
    manager.note_connected_client(2'000'007, client_generation);
    VERIFY(decoder_count() == 1);
    lost_client.clear();
    next_client.clear();
    manager.process_did_release_decoder(2'000'007, *client_generation);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });

    // A client that went with its decoder does not count against the decoder that comes next.
    client_generation.clear();
    Optional<IPC::TransportHandle> client_of_dead_decoder = MUST(manager.connect_new_client(2'000'008, session, site));
    manager.note_connected_client(2'000'008, client_generation);
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::ImageDecoder)
            manager.process_did_exit(process.child_id());
    });
    client_of_dead_decoder.clear();
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });
    Optional<IPC::TransportHandle> client_of_next_decoder = MUST(manager.connect_new_client(2'000'008, session, site));
    manager.note_connected_client(2'000'008, client_generation);
    VERIFY(decoder_count() == 1);
    client_of_next_decoder.clear();
    manager.process_did_release_decoder(2'000'008, *client_generation);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });

    // A process that has let go of its decoder gets a new one when it next asks.
    auto handle = MUST(manager.connect_new_client(2'000'003, session, {}));
    VERIFY(decoder_count() == 1);

    // The frame client of an animation only serves the client that connected it, so it does not keep the decoder.
    auto client = make_ref_counted<ImageDecoderClient::Client>(MUST(handle.create_transport()));
    auto frame_channel = MUST(IPC::TransportHandle::create_pair());
    client->async_connect_frame_client(frame_channel.first);
    auto frame_client = make_ref_counted<ImageDecoderClient::Client>(MUST(frame_channel.second.create_transport()));
    // NB: The decoder handles a client's messages in order, so the frame client is connected once this returns.
    auto image = MUST(decode_base64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7"sv));
    MUST(client->decode_first_frame_synchronously(image));
    client->shutdown();
    manager.process_did_release_decoder(2'000'003);
    Core::EventLoop::current().spin_until([&] { return decoder_count() == 0; });
    frame_client->shutdown();

    outln("PASS: image decoders end once the processes they serve let go of them");
}
