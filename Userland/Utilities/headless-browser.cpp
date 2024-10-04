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

static StringView s_current_test_path;

class HeadlessWebContentView final : public WebView::ViewImplementation {
public:
    static ErrorOr<NonnullOwnPtr<HeadlessWebContentView>> create(Core::AnonymousBuffer theme, Gfx::IntSize const& window_size, StringView resources_folder)
    {
        RefPtr<Requests::RequestClient> request_client;
        RefPtr<ImageDecoderClient::Client> image_decoder_client;

        auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
        request_client = TRY(launch_request_server_process(request_server_paths, resources_folder));

        auto image_decoder_paths = TRY(get_paths_for_helper_process("ImageDecoder"sv));
        image_decoder_client = TRY(launch_image_decoder_process(image_decoder_paths));

        auto view = TRY(adopt_nonnull_own_or_enomem(new (nothrow) HeadlessWebContentView(image_decoder_client, request_client)));

        auto request_server_socket = TRY(connect_new_request_server_client(*request_client));
        auto image_decoder_socket = TRY(connect_new_image_decoder_client(*image_decoder_client));

        auto candidate_web_content_paths = TRY(get_paths_for_helper_process("WebContent"sv));
        view->m_client_state.client = TRY(launch_web_content_process(*view, candidate_web_content_paths, move(image_decoder_socket), move(request_server_socket)));

        view->client().async_update_system_theme(0, move(theme));

        view->m_viewport_size = window_size;
        view->client().async_set_viewport_size(0, view->m_viewport_size.to_type<Web::DevicePixels>());
        view->client().async_set_window_size(0, window_size.to_type<Web::DevicePixels>());

        if (auto web_driver_ipc_path = WebView::Application::chrome_options().webdriver_content_ipc_path; web_driver_ipc_path.has_value())
            view->client().async_connect_to_webdriver(0, *web_driver_ipc_path);

        view->m_client_state.client->on_web_content_process_crash = [] {
            warnln("\033[31;1mWebContent Crashed!!\033[0m");
            if (!s_current_test_path.is_empty()) {
                warnln("    Last started test: {}", s_current_test_path);
            }
            VERIFY_NOT_REACHED();
        };

        return view;
    }

    RefPtr<Gfx::Bitmap> take_screenshot()
    {
        VERIFY(!m_pending_screenshot);

        m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap>>::construct();
        client().async_take_document_screenshot(0);

        auto screenshot = MUST(m_pending_screenshot->await());
        m_pending_screenshot = nullptr;

        return screenshot;
    }

    virtual void did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot) override
    {
        VERIFY(m_pending_screenshot);
        m_pending_screenshot->resolve(screenshot.bitmap());
    }

    void clear_content_filters()
    {
        client().async_set_content_filters(0, {});
    }

private:
    HeadlessWebContentView(RefPtr<ImageDecoderClient::Client> image_decoder_client, RefPtr<Requests::RequestClient> request_client)
        : m_request_client(move(request_client))
        , m_image_decoder_client(move(image_decoder_client))
    {
        on_request_worker_agent = [this]() {
            auto worker_client = MUST(launch_web_worker_process(MUST(get_paths_for_helper_process("WebWorker"sv)), *m_request_client));
            return worker_client->dup_socket();
        };
    }

    void update_zoom() override { }
    void initialize_client(CreateNewClient) override { }

    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size.to_type<Web::DevicePixels>(); }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

    Gfx::IntSize m_viewport_size;
    RefPtr<Core::Promise<RefPtr<Gfx::Bitmap>>> m_pending_screenshot;

    RefPtr<Requests::RequestClient> m_request_client;
    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;
};

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
            if (auto screenshot = view.take_screenshot()) {
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

static StringView test_result_to_string(TestResult result)
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

static ErrorOr<TestResult> run_dump_test(HeadlessWebContentView& view, URL::URL const& url, StringView expectation_path, TestMode mode, bool rebaseline = false, int timeout_in_milliseconds = DEFAULT_TIMEOUT_MS)
{
    Core::EventLoop loop;
    bool did_timeout = false;

    auto timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&] {
        did_timeout = true;
        loop.quit(0);
    });

    String result;
    auto did_finish_test = false;
    auto did_finish_loading = false;

    if (mode == TestMode::Layout) {
        view.on_load_finish = [&](auto const& loaded_url) {
            // This callback will be called for 'about:blank' first, then for the URL we actually want to dump
            VERIFY(url.equals(loaded_url, URL::ExcludeFragment::Yes) || loaded_url.equals(URL::URL("about:blank")));

            if (url.equals(loaded_url, URL::ExcludeFragment::Yes)) {
                // NOTE: We take a screenshot here to force the lazy layout of SVG-as-image documents to happen.
                //       It also causes a lot more code to run, which is good for finding bugs. :^)
                (void)view.take_screenshot();

                auto promise = view.request_internal_page_info(WebView::PageInfoType::LayoutTree | WebView::PageInfoType::PaintTree);
                result = MUST(promise->await());

                loop.quit(0);
            }
        };

        view.on_text_test_finish = {};
    } else if (mode == TestMode::Text) {
        view.on_load_finish = [&](auto const& loaded_url) {
            // NOTE: We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;
            did_finish_loading = true;
            if (did_finish_test)
                loop.quit(0);
        };

        view.on_text_test_finish = [&](auto const& text) {
            result = text;

            did_finish_test = true;
            if (did_finish_loading)
                loop.quit(0);
        };
    }

    view.load(url);

    timeout_timer->start();
    loop.exec();

    if (did_timeout)
        return TestResult::Timeout;

    if (expectation_path.is_empty()) {
        out("{}", result);
        return TestResult::Skipped;
    }

    auto expectation_file_or_error = Core::File::open(expectation_path, rebaseline ? Core::File::OpenMode::Write : Core::File::OpenMode::Read);
    if (expectation_file_or_error.is_error()) {
        warnln("Failed opening '{}': {}", expectation_path, expectation_file_or_error.error());
        return expectation_file_or_error.release_error();
    }

    auto expectation_file = expectation_file_or_error.release_value();

    if (rebaseline) {
        TRY(expectation_file->write_until_depleted(result));
        return TestResult::Pass;
    }

    auto expectation = TRY(String::from_utf8(StringView(TRY(expectation_file->read_until_eof()).bytes())));

    auto actual = result;
    auto actual_trimmed = TRY(actual.trim("\n"sv, TrimMode::Right));
    auto expectation_trimmed = TRY(expectation.trim("\n"sv, TrimMode::Right));

    if (actual_trimmed == expectation_trimmed)
        return TestResult::Pass;

    auto const color_output = isatty(STDOUT_FILENO) ? Diff::ColorOutput::Yes : Diff::ColorOutput::No;

    if (color_output == Diff::ColorOutput::Yes)
        outln("\n\033[33;1mTest failed\033[0m: {}", url);
    else
        outln("\nTest failed: {}", url);

    auto hunks = TRY(Diff::from_text(expectation, actual, 3));
    auto out = TRY(Core::File::standard_output());

    TRY(Diff::write_unified_header(expectation_path, expectation_path, *out));
    for (auto const& hunk : hunks)
        TRY(Diff::write_unified(hunk, *out, color_output));

    return TestResult::Fail;
}

static ErrorOr<TestResult> run_ref_test(HeadlessWebContentView& view, URL::URL const& url, bool dump_failed_ref_tests, int timeout_in_milliseconds = DEFAULT_TIMEOUT_MS)
{
    Core::EventLoop loop;
    bool did_timeout = false;

    auto timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&] {
        did_timeout = true;
        loop.quit(0);
    });

    RefPtr<Gfx::Bitmap> actual_screenshot, expectation_screenshot;
    view.on_load_finish = [&](auto const&) {
        if (actual_screenshot) {
            expectation_screenshot = view.take_screenshot();
            loop.quit(0);
        } else {
            actual_screenshot = view.take_screenshot();
            view.debug_request("load-reference-page");
        }
    };
    view.on_text_test_finish = [&](auto const&) {
        dbgln("Unexpected text test finished during ref test for {}", url);
    };

    view.load(url);

    timeout_timer->start();
    loop.exec();

    if (did_timeout)
        return TestResult::Timeout;

    VERIFY(actual_screenshot);
    VERIFY(expectation_screenshot);

    if (actual_screenshot->visually_equals(*expectation_screenshot))
        return TestResult::Pass;

    if (dump_failed_ref_tests) {
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
        TRY(dump_screenshot(*actual_screenshot, ByteString::formatted("test-dumps/{}.png", title)));
        TRY(dump_screenshot(*expectation_screenshot, ByteString::formatted("test-dumps/{}-ref.png", title)));
    }

    return TestResult::Fail;
}

static ErrorOr<TestResult> run_test(HeadlessWebContentView& view, StringView input_path, StringView expectation_path, TestMode mode, bool dump_failed_ref_tests, bool rebaseline)
{
    // Clear the current document.
    // FIXME: Implement a debug-request to do this more thoroughly.
    auto promise = Core::Promise<Empty>::construct();
    view.on_load_finish = [&](auto) {
        promise->resolve({});
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

    view.load(URL::URL("about:blank"sv));
    MUST(promise->await());

    auto url = URL::create_with_file_scheme(TRY(FileSystem::real_path(input_path)));
    s_current_test_path = input_path;

    switch (mode) {
    case TestMode::Text:
    case TestMode::Layout:
        return run_dump_test(view, url, expectation_path, mode, rebaseline);
    case TestMode::Ref:
        return run_ref_test(view, url, dump_failed_ref_tests);
    default:
        VERIFY_NOT_REACHED();
    }
}

struct Test {
    ByteString input_path;
    ByteString expectation_path;
    TestMode mode;
    Optional<TestResult> result;
};

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

        tests.append({ input_path, move(expectation_path), mode, {} });
    }
    return {};
}

static ErrorOr<void> collect_ref_tests(Vector<Test>& tests, StringView path)
{
    TRY(Core::Directory::for_each_entry(path, Core::DirIterator::SkipDots, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        if (entry.type == Core::DirectoryEntry::Type::Directory)
            return IterationDecision::Continue;
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/{}", path, entry.name)));
        tests.append({ input_path, {}, TestMode::Ref, {} });
        return IterationDecision::Continue;
    }));

    return {};
}

static ErrorOr<int> run_tests(HeadlessWebContentView* view, StringView test_root_path, StringView test_glob, bool dump_failed_ref_tests, bool dump_gc_graph, bool dry_run, bool rebaseline)
{
    if (view)
        view->clear_content_filters();

    TRY(load_test_config(test_root_path));

    Vector<Test> tests;
    TRY(collect_dump_tests(tests, ByteString::formatted("{}/Layout", test_root_path), "."sv, TestMode::Layout));
    TRY(collect_dump_tests(tests, ByteString::formatted("{}/Text", test_root_path), "."sv, TestMode::Text));
    TRY(collect_ref_tests(tests, ByteString::formatted("{}/Ref", test_root_path)));
#ifndef AK_OS_MACOS
    TRY(collect_ref_tests(tests, ByteString::formatted("{}/Screenshot", test_root_path)));
#endif

    tests.remove_all_matching([&](auto const& test) {
        return !test.input_path.matches(test_glob, CaseSensitivity::CaseSensitive);
    });

    size_t pass_count = 0;
    size_t fail_count = 0;
    size_t timeout_count = 0;
    size_t skipped_count = 0;

    bool is_tty = isatty(STDOUT_FILENO);

    if (dry_run)
        outln("Found {} tests...", tests.size());
    else
        outln("Running {} tests...", tests.size());

    for (size_t i = 0; i < tests.size(); ++i) {
        auto& test = tests[i];

        if (is_tty && !dry_run) {
            // Keep clearing and reusing the same line if stdout is a TTY.
            out("\33[2K\r");
        }

        out("{}/{}: {}", i + 1, tests.size(), LexicalPath::relative_path(test.input_path, test_root_path));

        if (dry_run) {
            outln("");
            continue;
        }

        if (is_tty)
            fflush(stdout);
        else
            outln("");

        if (s_skipped_tests.contains_slow(test.input_path)) {
            test.result = TestResult::Skipped;
            ++skipped_count;
            continue;
        }

        test.result = TRY(run_test(*view, test.input_path, test.expectation_path, test.mode, dump_failed_ref_tests, rebaseline));
        switch (*test.result) {
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
            VERIFY_NOT_REACHED();
            break;
        }
    }

    if (dry_run)
        return 0;

    if (is_tty)
        outln("\33[2K\rDone!");

    outln("==================================================");
    outln("Pass: {}, Fail: {}, Skipped: {}, Timeout: {}", pass_count, fail_count, skipped_count, timeout_count);
    outln("==================================================");
    for (auto& test : tests) {
        if (*test.result == TestResult::Pass)
            continue;
        outln("{}: {}", test_result_to_string(*test.result), test.input_path);
    }

    if (dump_gc_graph) {
        auto path = view->dump_gc_graph();
        if (path.is_error()) {
            warnln("Failed to dump GC graph: {}", path.error());
        } else {
            outln("GC graph dumped to {}", path.value());
        }
    }

    if (timeout_count == 0 && fail_count == 0)
        return 0;
    return 1;
}

struct Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

    virtual void create_platform_arguments(Core::ArgsParser& args_parser) override
    {
        args_parser.add_option(screenshot_timeout, "Take a screenshot after [n] seconds (default: 1)", "screenshot", 's', "n");
        args_parser.add_option(dump_layout_tree, "Dump layout tree and exit", "dump-layout-tree", 'd');
        args_parser.add_option(dump_text, "Dump text and exit", "dump-text", 'T');
        args_parser.add_option(test_root_path, "Run tests in path", "run-tests", 'R', "test-root-path");
        args_parser.add_option(test_glob, "Only run tests matching the given glob", "filter", 'f', "glob");
        args_parser.add_option(test_dry_run, "List the tests that would be run, without running them", "dry-run");
        args_parser.add_option(dump_failed_ref_tests, "Dump screenshots of failing ref tests", "dump-failed-ref-tests", 'D');
        args_parser.add_option(dump_gc_graph, "Dump GC graph", "dump-gc-graph", 'G');
        args_parser.add_option(resources_folder, "Path of the base resources folder (defaults to /res)", "resources", 'r', "resources-root-path");
        args_parser.add_option(is_layout_test_mode, "Enable layout test mode", "layout-test-mode");
        args_parser.add_option(rebaseline, "Rebaseline any executed layout or text tests", "rebaseline");
    }

    virtual void create_platform_options(WebView::ChromeOptions&, WebView::WebContentOptions& web_content_options) override
    {
        if (!test_root_path.is_empty()) {
            // --run-tests implies --layout-test-mode.
            is_layout_test_mode = true;
        }

        web_content_options.is_layout_test_mode = is_layout_test_mode ? WebView::IsLayoutTestMode::Yes : WebView::IsLayoutTestMode::No;
    }

    int screenshot_timeout { 1 };
    ByteString resources_folder { s_ladybird_resource_root };
    bool dump_failed_ref_tests { false };
    bool dump_layout_tree { false };
    bool dump_text { false };
    bool dump_gc_graph { false };
    bool is_layout_test_mode { false };
    StringView test_root_path;
    ByteString test_glob;
    bool test_dry_run { false };
    bool rebaseline { false };
};

Application::Application(Badge<WebView::Application>, Main::Arguments&)
{
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    platform_init();

    auto app = Application::create(arguments, "about:newtab"sv);

    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(app->resources_folder))));

    auto theme_path = LexicalPath::join(app->resources_folder, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    // FIXME: Allow passing the window size as an argument.
    static constexpr Gfx::IntSize window_size { 800, 600 };

    if (!app->test_root_path.is_empty()) {
        OwnPtr<HeadlessWebContentView> view;
        if (!app->test_dry_run)
            view = TRY(HeadlessWebContentView::create(move(theme), window_size, app->resources_folder));

        auto absolute_test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);
        app->test_root_path = absolute_test_root_path;
        auto test_glob = ByteString::formatted("*{}*", app->test_glob);
        return run_tests(view, app->test_root_path, test_glob, app->dump_failed_ref_tests, app->dump_gc_graph, app->test_dry_run, app->rebaseline);
    }

    auto view = TRY(HeadlessWebContentView::create(move(theme), window_size, app->resources_folder));

    VERIFY(!WebView::Application::chrome_options().urls.is_empty());
    auto const& url = WebView::Application::chrome_options().urls.first();
    if (!url.is_valid()) {
        warnln("Invalid URL: \"{}\"", url);
        return Error::from_string_literal("Invalid URL");
    }

    if (app->dump_layout_tree) {
        TRY(run_dump_test(*view, url, ""sv, TestMode::Layout));
        return 0;
    }

    if (app->dump_text) {
        TRY(run_dump_test(*view, url, ""sv, TestMode::Text));
        return 0;
    }

    if (!WebView::Application::chrome_options().webdriver_content_ipc_path.has_value()) {
        auto timer = TRY(load_page_for_screenshot_and_exit(Core::EventLoop::current(), *view, url, app->screenshot_timeout));
        return app->execute();
    }

    return 0;
}
