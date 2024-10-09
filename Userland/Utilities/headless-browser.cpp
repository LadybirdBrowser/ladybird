/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Enumerate.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Promise.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Point.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibGfx/SystemTheme.h>
#include <LibIPC/File.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

constexpr int DEFAULT_TIMEOUT_MS = 30000; // 30sec

enum class TestMode {
    Layout,
    Text,
    Ref,
};

enum class TestResult {
    Pass,
    Fail,
    Skipped,
    Timeout,
};

static constexpr StringView test_result_to_string(TestResult result)
{
    switch (result) {
    case TestResult::Pass:
        return "Pass"sv;
    case TestResult::Fail:
        return "Fail"sv;
    case TestResult::Skipped:
        return "Skipped"sv;
    case TestResult::Timeout:
        return "Timeout"sv;
    }
    VERIFY_NOT_REACHED();
}

struct Test {
    TestMode mode;

    ByteString input_path {};
    ByteString expectation_path {};

    String text {};
    bool did_finish_test { false };
    bool did_finish_loading { false };

    RefPtr<Gfx::Bitmap> actual_screenshot {};
    RefPtr<Gfx::Bitmap> expectation_screenshot {};
};

struct TestCompletion {
    Test& test;
    TestResult result;
};

using TestPromise = Core::Promise<TestCompletion>;

class HeadlessWebContentView;

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    static Application& the()
    {
        return static_cast<Application&>(WebView::Application::the());
    }

    virtual void create_platform_arguments(Core::ArgsParser& args_parser) override
    {
        args_parser.add_option(screenshot_timeout, "Take a screenshot after [n] seconds (default: 1)", "screenshot", 's', "n");
        args_parser.add_option(dump_layout_tree, "Dump layout tree and exit", "dump-layout-tree", 'd');
        args_parser.add_option(dump_text, "Dump text and exit", "dump-text", 'T');
        args_parser.add_option(test_concurrency, "Maximum number of tests to run at once", "test-concurrency", 'j', "jobs");
        args_parser.add_option(test_root_path, "Run tests in path", "run-tests", 'R', "test-root-path");
        args_parser.add_option(test_glob, "Only run tests matching the given glob", "filter", 'f', "glob");
        args_parser.add_option(test_dry_run, "List the tests that would be run, without running them", "dry-run");
        args_parser.add_option(dump_failed_ref_tests, "Dump screenshots of failing ref tests", "dump-failed-ref-tests", 'D');
        args_parser.add_option(dump_gc_graph, "Dump GC graph", "dump-gc-graph", 'G');
        args_parser.add_option(resources_folder, "Path of the base resources folder (defaults to /res)", "resources", 'r', "resources-root-path");
        args_parser.add_option(is_layout_test_mode, "Enable layout test mode", "layout-test-mode");
        args_parser.add_option(rebaseline, "Rebaseline any executed layout or text tests", "rebaseline");
    }

    virtual void create_platform_options(WebView::ChromeOptions& chrome_options, WebView::WebContentOptions& web_content_options) override
    {
        if (!test_root_path.is_empty()) {
            // --run-tests implies --layout-test-mode.
            is_layout_test_mode = true;
        }

        if (is_layout_test_mode) {
            // Allow window.open() to succeed for tests.
            chrome_options.allow_popups = WebView::AllowPopups::Yes;
        }

        if (dump_gc_graph) {
            // Force all tests to run in serial if we are interested in the GC graph.
            test_concurrency = 1;
        }

        web_content_options.is_layout_test_mode = is_layout_test_mode ? WebView::IsLayoutTestMode::Yes : WebView::IsLayoutTestMode::No;
    }

    ErrorOr<void> launch_services()
    {
        auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
        m_request_client = TRY(launch_request_server_process(request_server_paths, resources_folder));

        auto image_decoder_paths = TRY(get_paths_for_helper_process("ImageDecoder"sv));
        m_image_decoder_client = TRY(launch_image_decoder_process(image_decoder_paths));

        return {};
    }

    static Requests::RequestClient& request_client() { return *the().m_request_client; }
    static ImageDecoderClient::Client& image_decoder_client() { return *the().m_image_decoder_client; }

    ErrorOr<HeadlessWebContentView*> create_web_view(Core::AnonymousBuffer theme, Gfx::IntSize window_size);
    void destroy_web_views();

    template<typename Callback>
    void for_each_web_view(Callback&& callback)
    {
        for (auto& web_view : m_web_views)
            callback(*web_view);
    }

    int screenshot_timeout { 1 };
    ByteString resources_folder { s_ladybird_resource_root };
    bool dump_failed_ref_tests { false };
    bool dump_layout_tree { false };
    bool dump_text { false };
    bool dump_gc_graph { false };
    bool is_layout_test_mode { false };
    size_t test_concurrency { Core::System::hardware_concurrency() };
    ByteString test_root_path;
    ByteString test_glob;
    bool test_dry_run { false };
    bool rebaseline { false };

private:
    RefPtr<Requests::RequestClient> m_request_client;
    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;

    Vector<NonnullOwnPtr<HeadlessWebContentView>> m_web_views;
};

Application::Application(Badge<WebView::Application>, Main::Arguments&)
{
}

class HeadlessWebContentView final : public WebView::ViewImplementation {
public:
    static ErrorOr<NonnullOwnPtr<HeadlessWebContentView>> create(Core::AnonymousBuffer theme, Gfx::IntSize window_size)
    {
        auto view = TRY(adopt_nonnull_own_or_enomem(new (nothrow) HeadlessWebContentView(window_size)));

        auto request_server_socket = TRY(connect_new_request_server_client(Application::request_client()));
        auto image_decoder_socket = TRY(connect_new_image_decoder_client(Application::image_decoder_client()));

        auto candidate_web_content_paths = TRY(get_paths_for_helper_process("WebContent"sv));
        view->m_client_state.client = TRY(launch_web_content_process(*view, candidate_web_content_paths, move(image_decoder_socket), move(request_server_socket)));

        view->client().async_update_system_theme(0, move(theme));
        view->client().async_set_viewport_size(0, view->viewport_size());
        view->client().async_set_window_size(0, view->viewport_size());

        if (WebView::Application::chrome_options().allow_popups == WebView::AllowPopups::Yes)
            view->client().async_debug_request(0, "block-pop-ups"sv, "off"sv);

        if (auto web_driver_ipc_path = WebView::Application::chrome_options().webdriver_content_ipc_path; web_driver_ipc_path.has_value())
            view->client().async_connect_to_webdriver(0, *web_driver_ipc_path);

        view->m_client_state.client->on_web_content_process_crash = [&view = *view] {
            warnln("\033[31;1mWebContent Crashed!!\033[0m");
            warnln("    Last page loaded: {}", view.url());
            VERIFY_NOT_REACHED();
        };

        return view;
    }

    NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap>>> take_screenshot()
    {
        VERIFY(!m_pending_screenshot);

        m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap>>::construct();
        client().async_take_document_screenshot(0);

        return *m_pending_screenshot;
    }

    void clear_content_filters()
    {
        client().async_set_content_filters(0, {});
    }

    TestPromise& test_promise() { return *m_test_promise; }

    void on_test_complete(TestCompletion completion)
    {
        m_test_promise->resolve(move(completion));
    }

private:
    HeadlessWebContentView(Gfx::IntSize viewport_size)
        : m_viewport_size(viewport_size)
        , m_test_promise(TestPromise::construct())
    {
        on_request_worker_agent = []() {
            auto worker_client = MUST(launch_web_worker_process(MUST(get_paths_for_helper_process("WebWorker"sv)), Application::request_client()));
            return worker_client->dup_socket();
        };
    }

    void update_zoom() override { }
    void initialize_client(CreateNewClient) override { }

    virtual void did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot) override
    {
        VERIFY(m_pending_screenshot);

        auto pending_screenshot = move(m_pending_screenshot);
        pending_screenshot->resolve(screenshot.bitmap());
    }

    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size.to_type<Web::DevicePixels>(); }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

    Gfx::IntSize m_viewport_size;
    RefPtr<Core::Promise<RefPtr<Gfx::Bitmap>>> m_pending_screenshot;

    NonnullRefPtr<TestPromise> m_test_promise;
};

ErrorOr<HeadlessWebContentView*> Application::create_web_view(Core::AnonymousBuffer theme, Gfx::IntSize window_size)
{
    auto web_view = TRY(HeadlessWebContentView::create(move(theme), window_size));
    m_web_views.append(move(web_view));

    return m_web_views.last().ptr();
}

void Application::destroy_web_views()
{
    m_web_views.clear();
}

static ErrorOr<NonnullRefPtr<Core::Timer>> load_page_for_screenshot_and_exit(Core::EventLoop& event_loop, HeadlessWebContentView& view, URL::URL const& url, int screenshot_timeout)
{
    // FIXME: Allow passing the output path as an argument.
    static constexpr auto output_file_path = "output.png"sv;

    if (FileSystem::exists(output_file_path))
        TRY(FileSystem::remove(output_file_path, FileSystem::RecursionMode::Disallowed));

    outln("Taking screenshot after {} seconds", screenshot_timeout);

    auto timer = Core::Timer::create_single_shot(
        screenshot_timeout * 1000,
        [&]() {
            auto promise = view.take_screenshot();

            if (auto screenshot = MUST(promise->await())) {
                outln("Saving screenshot to {}", output_file_path);

                auto output_file = MUST(Core::File::open(output_file_path, Core::File::OpenMode::Write));
                auto image_buffer = MUST(Gfx::PNGWriter::encode(*screenshot));
                MUST(output_file->write_until_depleted(image_buffer.bytes()));
            } else {
                warnln("No screenshot available");
            }

            event_loop.quit(0);
        });

    view.load(url);
    timer->start();
    return timer;
}

static void run_dump_test(HeadlessWebContentView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds = DEFAULT_TIMEOUT_MS)
{
    auto timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, &test]() {
        view.on_load_finish = {};
        view.on_text_test_finish = {};

        view.on_test_complete({ test, TestResult::Timeout });
    });

    auto handle_completed_test = [&test, url]() -> ErrorOr<TestResult> {
        if (test.expectation_path.is_empty()) {
            outln("{}", test.text);
            return TestResult::Pass;
        }

        auto open_expectation_file = [&](auto mode) {
            auto expectation_file_or_error = Core::File::open(test.expectation_path, mode);
            if (expectation_file_or_error.is_error())
                warnln("Failed opening '{}': {}", test.expectation_path, expectation_file_or_error.error());

            return expectation_file_or_error;
        };

        ByteBuffer expectation;

        if (auto expectation_file = open_expectation_file(Core::File::OpenMode::Read); !expectation_file.is_error()) {
            expectation = TRY(expectation_file.value()->read_until_eof());

            auto result_trimmed = StringView { test.text }.trim("\n"sv, TrimMode::Right);
            auto expectation_trimmed = StringView { expectation }.trim("\n"sv, TrimMode::Right);

            if (result_trimmed == expectation_trimmed)
                return TestResult::Pass;
        } else if (!Application::the().rebaseline) {
            return expectation_file.release_error();
        }

        if (Application::the().rebaseline) {
            auto expectation_file = TRY(open_expectation_file(Core::File::OpenMode::Write));
            TRY(expectation_file->write_until_depleted(test.text));

            return TestResult::Pass;
        }

        auto const color_output = isatty(STDOUT_FILENO) ? Diff::ColorOutput::Yes : Diff::ColorOutput::No;

        if (color_output == Diff::ColorOutput::Yes)
            outln("\n\033[33;1mTest failed\033[0m: {}", url);
        else
            outln("\nTest failed: {}", url);

        auto hunks = TRY(Diff::from_text(expectation, test.text, 3));
        auto out = TRY(Core::File::standard_output());

        TRY(Diff::write_unified_header(test.expectation_path, test.expectation_path, *out));
        for (auto const& hunk : hunks)
            TRY(Diff::write_unified(hunk, *out, color_output));

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, timer, handle_completed_test]() {
        timer->stop();

        view.on_load_finish = {};
        view.on_text_test_finish = {};

        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    if (test.mode == TestMode::Layout) {
        view.on_load_finish = [&view, &test, url, on_test_complete = move(on_test_complete)](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            // NOTE: We take a screenshot here to force the lazy layout of SVG-as-image documents to happen.
            //       It also causes a lot more code to run, which is good for finding bugs. :^)
            view.take_screenshot()->when_resolved([&view, &test, on_test_complete = move(on_test_complete)](auto) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::LayoutTree | WebView::PageInfoType::PaintTree);

                promise->when_resolved([&test, on_test_complete = move(on_test_complete)](auto const& text) {
                    test.text = text;
                    on_test_complete();
                });
            });
        };
    } else if (test.mode == TestMode::Text) {
        view.on_load_finish = [&view, &test, on_test_complete, url](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            test.did_finish_loading = true;

            if (test.expectation_path.is_empty()) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::Text);

                promise->when_resolved([&test, on_test_complete = move(on_test_complete)](auto const& text) {
                    test.text = text;
                    on_test_complete();
                });
            } else if (test.did_finish_test) {
                on_test_complete();
            }
        };

        view.on_text_test_finish = [&test, on_test_complete](auto const& text) {
            test.text = text;
            test.did_finish_test = true;

            if (test.did_finish_loading)
                on_test_complete();
        };
    }

    view.load(url);
    timer->start();
}

static void run_ref_test(HeadlessWebContentView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds = DEFAULT_TIMEOUT_MS)
{
    auto timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, &test]() {
        view.on_load_finish = {};
        view.on_text_test_finish = {};

        view.on_test_complete({ test, TestResult::Timeout });
    });

    auto handle_completed_test = [&test, url]() -> ErrorOr<TestResult> {
        if (test.actual_screenshot->visually_equals(*test.expectation_screenshot))
            return TestResult::Pass;

        if (Application::the().dump_failed_ref_tests) {
            warnln("\033[33;1mRef test {} failed; dumping screenshots\033[0m", url);
            auto title = LexicalPath::title(URL::percent_decode(url.serialize_path()));
            auto dump_screenshot = [&](Gfx::Bitmap& bitmap, StringView path) -> ErrorOr<void> {
                auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
                auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
                TRY(screenshot_file->write_until_depleted(encoded_data));
                warnln("\033[33;1mDumped {}\033[0m", TRY(FileSystem::real_path(path)));
                return {};
            };

            auto mkdir_result = Core::System::mkdir("test-dumps"sv, 0755);
            if (mkdir_result.is_error() && mkdir_result.error().code() != EEXIST)
                return mkdir_result.release_error();

            TRY(dump_screenshot(*test.actual_screenshot, ByteString::formatted("test-dumps/{}.png", title)));
            TRY(dump_screenshot(*test.expectation_screenshot, ByteString::formatted("test-dumps/{}-ref.png", title)));
        }

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, timer, handle_completed_test]() {
        timer->stop();

        view.on_load_finish = {};
        view.on_text_test_finish = {};

        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    view.on_load_finish = [&view, &test, on_test_complete = move(on_test_complete)](auto const&) {
        if (test.actual_screenshot) {
            view.take_screenshot()->when_resolved([&test, on_test_complete = move(on_test_complete)](RefPtr<Gfx::Bitmap> screenshot) {
                test.expectation_screenshot = move(screenshot);
                on_test_complete();
            });
        } else {
            view.take_screenshot()->when_resolved([&view, &test](RefPtr<Gfx::Bitmap> screenshot) {
                test.actual_screenshot = move(screenshot);
                view.debug_request("load-reference-page");
            });
        }
    };

    view.on_text_test_finish = [&](auto const&) {
        dbgln("Unexpected text test finished during ref test for {}", url);
    };

    view.load(url);
    timer->start();
}

static void run_test(HeadlessWebContentView& view, Test& test)
{
    // Clear the current document.
    // FIXME: Implement a debug-request to do this more thoroughly.
    auto promise = Core::Promise<Empty>::construct();

    view.on_load_finish = [promise](auto const& url) {
        if (!url.equals("about:blank"sv))
            return;

        Core::deferred_invoke([promise]() {
            promise->resolve({});
        });
    };

    view.on_text_test_finish = {};

    view.on_request_file_picker = [&](auto const& accepted_file_types, auto allow_multiple_files) {
        // Create some dummy files for tests.
        Vector<Web::HTML::SelectedFile> selected_files;

        bool add_txt_files = accepted_file_types.filters.is_empty();
        bool add_cpp_files = false;

        for (auto const& filter : accepted_file_types.filters) {
            filter.visit(
                [](Web::HTML::FileFilter::FileType) {},
                [&](Web::HTML::FileFilter::MimeType const& mime_type) {
                    if (mime_type.value == "text/plain"sv)
                        add_txt_files = true;
                },
                [&](Web::HTML::FileFilter::Extension const& extension) {
                    if (extension.value == "cpp"sv)
                        add_cpp_files = true;
                });
        }

        if (add_txt_files) {
            selected_files.empend("file1"sv, MUST(ByteBuffer::copy("Contents for file1"sv.bytes())));

            if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
                selected_files.empend("file2"sv, MUST(ByteBuffer::copy("Contents for file2"sv.bytes())));
                selected_files.empend("file3"sv, MUST(ByteBuffer::copy("Contents for file3"sv.bytes())));
                selected_files.empend("file4"sv, MUST(ByteBuffer::copy("Contents for file4"sv.bytes())));
            }
        }

        if (add_cpp_files) {
            selected_files.empend("file1.cpp"sv, MUST(ByteBuffer::copy("int main() {{ return 1; }}"sv.bytes())));

            if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
                selected_files.empend("file2.cpp"sv, MUST(ByteBuffer::copy("int main() {{ return 2; }}"sv.bytes())));
            }
        }

        view.file_picker_closed(move(selected_files));
    };

    promise->when_resolved([&view, &test](auto) {
        auto url = URL::create_with_file_scheme(MUST(FileSystem::real_path(test.input_path)));

        switch (test.mode) {
        case TestMode::Text:
        case TestMode::Layout:
            run_dump_test(view, test, url);
            return;
        case TestMode::Ref:
            run_ref_test(view, test, url);
            return;
        }

        VERIFY_NOT_REACHED();
    });

    view.load("about:blank"sv);
}

static Vector<ByteString> s_skipped_tests;

static ErrorOr<void> load_test_config(StringView test_root_path)
{
    auto config_path = LexicalPath::join(test_root_path, "TestConfig.ini"sv);
    auto config_or_error = Core::ConfigFile::open(config_path.string());

    if (config_or_error.is_error()) {
        if (config_or_error.error().code() == ENOENT)
            return {};
        dbgln("Unable to open test config {}", config_path);
        return config_or_error.release_error();
    }

    auto config = config_or_error.release_value();

    for (auto const& group : config->groups()) {
        if (group == "Skipped"sv) {
            for (auto& key : config->keys(group))
                s_skipped_tests.append(LexicalPath::join(test_root_path, key).string());
        } else {
            warnln("Unknown group '{}' in config {}", group, config_path);
        }
    }
    return {};
}

static ErrorOr<void> collect_dump_tests(Vector<Test>& tests, StringView path, StringView trail, TestMode mode)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));
        if (FileSystem::is_directory(input_path)) {
            TRY(collect_dump_tests(tests, path, ByteString::formatted("{}/{}", trail, name), mode));
            continue;
        }
        if (!name.ends_with(".html"sv) && !name.ends_with(".svg"sv))
            continue;
        auto basename = LexicalPath::title(name);
        auto expectation_path = ByteString::formatted("{}/expected/{}/{}.txt", path, trail, basename);

        tests.append({ mode, input_path, move(expectation_path), {} });
    }
    return {};
}

static ErrorOr<void> collect_ref_tests(Vector<Test>& tests, StringView path)
{
    TRY(Core::Directory::for_each_entry(path, Core::DirIterator::SkipDots, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        if (entry.type == Core::DirectoryEntry::Type::Directory)
            return IterationDecision::Continue;
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/{}", path, entry.name)));
        tests.append({ TestMode::Ref, input_path, {}, {} });
        return IterationDecision::Continue;
    }));

    return {};
}

static ErrorOr<int> run_tests(Core::AnonymousBuffer const& theme, Gfx::IntSize window_size)
{
    auto& app = Application::the();
    TRY(load_test_config(app.test_root_path));

    Vector<Test> tests;
    auto test_glob = ByteString::formatted("*{}*", app.test_glob);

    TRY(collect_dump_tests(tests, ByteString::formatted("{}/Layout", app.test_root_path), "."sv, TestMode::Layout));
    TRY(collect_dump_tests(tests, ByteString::formatted("{}/Text", app.test_root_path), "."sv, TestMode::Text));
    TRY(collect_ref_tests(tests, ByteString::formatted("{}/Ref", app.test_root_path)));
#ifndef AK_OS_MACOS
    TRY(collect_ref_tests(tests, ByteString::formatted("{}/Screenshot", app.test_root_path)));
#endif

    tests.remove_all_matching([&](auto const& test) {
        return !test.input_path.matches(test_glob, CaseSensitivity::CaseSensitive);
    });

    if (app.test_dry_run) {
        outln("Found {} tests...", tests.size());

        for (auto const& [i, test] : enumerate(tests))
            outln("{}/{}: {}", i + 1, tests.size(), LexicalPath::relative_path(test.input_path, app.test_root_path));

        return 0;
    }

    auto concurrency = min(app.test_concurrency, tests.size());
    size_t loaded_web_views = 0;

    for (size_t i = 0; i < concurrency; ++i) {
        auto& view = *TRY(app.create_web_view(theme, window_size));
        view.on_load_finish = [&](auto const&) { ++loaded_web_views; };
    }

    // We need to wait for the initial about:blank load to complete before starting the tests, otherwise we may load the
    // test URL before the about:blank load completes. WebContent currently cannot handle this, and will drop the test URL.
    Core::EventLoop::current().spin_until([&]() {
        return loaded_web_views == concurrency;
    });

    size_t pass_count = 0;
    size_t fail_count = 0;
    size_t timeout_count = 0;
    size_t skipped_count = 0;

    bool is_tty = isatty(STDOUT_FILENO);
    outln("Running {} tests...", tests.size());

    auto all_tests_complete = Core::Promise<Empty>::construct();
    auto tests_remaining = tests.size();
    auto current_test = 0uz;

    Vector<TestCompletion> non_passing_tests;

    app.for_each_web_view([&](auto& view) {
        view.clear_content_filters();

        auto run_next_test = [&]() {
            auto index = current_test++;
            if (index >= tests.size())
                return;
            auto& test = tests[index];

            if (is_tty) {
                // Keep clearing and reusing the same line if stdout is a TTY.
                out("\33[2K\r");
            }

            out("{}/{}: {}", index + 1, tests.size(), LexicalPath::relative_path(test.input_path, app.test_root_path));

            if (is_tty)
                fflush(stdout);
            else
                outln("");

            Core::deferred_invoke([&]() mutable {
                if (s_skipped_tests.contains_slow(test.input_path))
                    view.on_test_complete({ test, TestResult::Skipped });
                else
                    run_test(view, test);
            });
        };

        view.test_promise().when_resolved([&, run_next_test](auto result) {
            switch (result.result) {
            case TestResult::Pass:
                ++pass_count;
                break;
            case TestResult::Fail:
                ++fail_count;
                break;
            case TestResult::Timeout:
                ++timeout_count;
                break;
            case TestResult::Skipped:
                ++skipped_count;
                break;
            }

            if (result.result != TestResult::Pass)
                non_passing_tests.append(move(result));

            if (--tests_remaining == 0)
                all_tests_complete->resolve({});
            else
                run_next_test();
        });

        Core::deferred_invoke([run_next_test]() {
            run_next_test();
        });
    });

    MUST(all_tests_complete->await());

    if (is_tty)
        outln("\33[2K\rDone!");

    outln("==================================================");
    outln("Pass: {}, Fail: {}, Skipped: {}, Timeout: {}", pass_count, fail_count, skipped_count, timeout_count);
    outln("==================================================");

    for (auto const& non_passing_test : non_passing_tests)
        outln("{}: {}", test_result_to_string(non_passing_test.result), non_passing_test.test.input_path);

    if (app.dump_gc_graph) {
        app.for_each_web_view([&](auto& view) {
            if (auto path = view.dump_gc_graph(); path.is_error())
                warnln("Failed to dump GC graph: {}", path.error());
            else
                outln("GC graph dumped to {}", path.value());
        });
    }

    app.destroy_web_views();

    if (timeout_count == 0 && fail_count == 0)
        return 0;
    return 1;
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    platform_init();

    auto app = Application::create(arguments, "about:newtab"sv);
    TRY(app->launch_services());

    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(app->resources_folder))));

    auto theme_path = LexicalPath::join(app->resources_folder, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    // FIXME: Allow passing the window size as an argument.
    static constexpr Gfx::IntSize window_size { 800, 600 };

    if (!app->test_root_path.is_empty()) {
        app->test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);
        return run_tests(theme, window_size);
    }

    auto& view = *TRY(app->create_web_view(move(theme), window_size));

    VERIFY(!WebView::Application::chrome_options().urls.is_empty());
    auto const& url = WebView::Application::chrome_options().urls.first();
    if (!url.is_valid()) {
        warnln("Invalid URL: \"{}\"", url);
        return Error::from_string_literal("Invalid URL");
    }

    if (app->dump_layout_tree || app->dump_text) {
        Test test { app->dump_layout_tree ? TestMode::Layout : TestMode::Text };
        run_dump_test(view, test, url);

        auto completion = MUST(view.test_promise().await());
        return completion.result == TestResult::Pass ? 0 : 1;
    }

    if (!WebView::Application::chrome_options().webdriver_content_ipc_path.has_value()) {
        auto timer = TRY(load_page_for_screenshot_and_exit(Core::EventLoop::current(), view, url, app->screenshot_timeout));
        return app->execute();
    }

    return 0;
}
