/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Enumerate.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <UI/Headless/Application.h>
#include <UI/Headless/HeadlessWebView.h>
#include <UI/Headless/Test.h>

namespace Ladybird {

static Vector<ByteString> s_skipped_tests;

static ErrorOr<void> load_test_config(StringView test_root_path)
{
    auto config_path = LexicalPath::join(test_root_path, "TestConfig.ini"sv);
    auto config_or_error = Core::ConfigFile::open(config_path.string());

    if (config_or_error.is_error()) {
        if (config_or_error.error().code() == ENOENT)
            return {};
        warnln("Unable to open test config {}", config_path);
        return config_or_error.release_error();
    }

    auto config = config_or_error.release_value();

    for (auto const& group : config->groups()) {
        if (group == "Skipped"sv) {
            for (auto& key : config->keys(group))
                s_skipped_tests.append(TRY(FileSystem::real_path(LexicalPath::join(test_root_path, key).string())));
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

        if (!name.ends_with(".html"sv) && !name.ends_with(".svg"sv) && !name.ends_with(".xhtml"sv) && !name.ends_with(".xht"sv))
            continue;

        auto expectation_path = ByteString::formatted("{}/expected/{}/{}.txt", path, trail, LexicalPath::title(name));
        tests.append({ mode, input_path, move(expectation_path), {} });
    }

    return {};
}

static ErrorOr<void> collect_ref_tests(Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_ref_tests(tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }

        tests.append({ TestMode::Ref, input_path, {}, {} });
    }

    return {};
}

static void clear_test_callbacks(HeadlessWebView& view)
{
    view.on_load_finish = {};
    view.on_text_test_finish = {};
    view.on_web_content_crashed = {};
}

void run_dump_test(HeadlessWebView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds)
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
            TRY(Core::Directory::create(LexicalPath { test.expectation_path }.parent().string(), Core::Directory::CreateDirectories::Yes));

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
        clear_test_callbacks(view);
        timer->stop();

        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    view.on_web_content_crashed = [&view, &test, timer]() {
        clear_test_callbacks(view);
        timer->stop();

        view.on_test_complete({ test, TestResult::Crashed });
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

static void run_ref_test(HeadlessWebView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds)
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

            auto dump_screenshot = [&](Gfx::Bitmap& bitmap, StringView path) -> ErrorOr<void> {
                auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
                auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
                TRY(screenshot_file->write_until_depleted(encoded_data));

                outln("\033[33;1mDumped {}\033[0m", TRY(FileSystem::real_path(path)));
                return {};
            };

            TRY(Core::Directory::create("test-dumps"sv, Core::Directory::CreateDirectories::Yes));

            auto title = LexicalPath::title(URL::percent_decode(url.serialize_path()));
            TRY(dump_screenshot(*test.actual_screenshot, ByteString::formatted("test-dumps/{}.png", title)));
            TRY(dump_screenshot(*test.expectation_screenshot, ByteString::formatted("test-dumps/{}-ref.png", title)));
        }

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, timer, handle_completed_test]() {
        clear_test_callbacks(view);
        timer->stop();

        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    view.on_web_content_crashed = [&view, &test, timer]() {
        clear_test_callbacks(view);
        timer->stop();

        view.on_test_complete({ test, TestResult::Crashed });
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

static void run_test(HeadlessWebView& view, Test& test, Application& app)
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

    promise->when_resolved([&view, &test, &app](auto) {
        auto url = URL::create_with_file_scheme(MUST(FileSystem::real_path(test.input_path)));

        switch (test.mode) {
        case TestMode::Text:
        case TestMode::Layout:
            run_dump_test(view, test, url, app.per_test_timeout_in_seconds * 1000);
            return;
        case TestMode::Ref:
            run_ref_test(view, test, url, app.per_test_timeout_in_seconds * 1000);
            return;
        }

        VERIFY_NOT_REACHED();
    });

    view.load("about:blank"sv);
}

ErrorOr<void> run_tests(Core::AnonymousBuffer const& theme, Web::DevicePixelSize window_size)
{
    auto& app = Application::the();
    TRY(load_test_config(app.test_root_path));

    Vector<Test> tests;
    auto test_glob = ByteString::formatted("*{}*", app.test_glob);

    TRY(collect_dump_tests(tests, ByteString::formatted("{}/Layout", app.test_root_path), "."sv, TestMode::Layout));
    TRY(collect_dump_tests(tests, ByteString::formatted("{}/Text", app.test_root_path), "."sv, TestMode::Text));
    TRY(collect_ref_tests(tests, ByteString::formatted("{}/Ref", app.test_root_path), "."sv));
#if !defined(AK_OS_MACOS)
    TRY(collect_ref_tests(tests, ByteString::formatted("{}/Screenshot", app.test_root_path), "."sv));
#endif

    tests.remove_all_matching([&](auto const& test) {
        return !test.input_path.matches(test_glob, CaseSensitivity::CaseSensitive);
    });

    if (app.test_dry_run) {
        outln("Found {} tests...", tests.size());

        for (auto const& [i, test] : enumerate(tests))
            outln("{}/{}: {}", i + 1, tests.size(), *LexicalPath::relative_path(test.input_path, app.test_root_path));

        return {};
    }

    if (tests.is_empty()) {
        if (app.test_glob.is_empty())
            return Error::from_string_literal("No tests found");
        return Error::from_string_literal("No tests found matching filter");
    }

    auto concurrency = min(app.test_concurrency, tests.size());
    size_t loaded_web_views = 0;

    for (size_t i = 0; i < concurrency; ++i) {
        auto& view = app.create_web_view(theme, window_size);
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
    size_t crashed_count = 0;
    size_t skipped_count = 0;
    bool all_tests_ok = true;

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
            test.start_time = UnixDateTime::now();

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
                    run_test(view, test, app);
            });
        };

        view.test_promise().when_resolved([&, run_next_test](auto result) {
            result.test.end_time = UnixDateTime::now();

            switch (result.result) {
            case TestResult::Pass:
                ++pass_count;
                break;
            case TestResult::Fail:
                all_tests_ok = false;
                ++fail_count;
                break;
            case TestResult::Timeout:
                all_tests_ok = false;
                ++timeout_count;
                break;
            case TestResult::Crashed:
                all_tests_ok = false;
                ++crashed_count;
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

    outln("==========================================================");
    outln("Pass: {}, Fail: {}, Skipped: {}, Timeout: {}, Crashed: {}", pass_count, fail_count, skipped_count, timeout_count, crashed_count);
    outln("==========================================================");

    for (auto const& non_passing_test : non_passing_tests) {
        if (non_passing_test.result == TestResult::Skipped && !app.verbose)
            continue;

        outln("{}: {}", test_result_to_string(non_passing_test.result), non_passing_test.test.input_path);
    }

    if (app.verbose) {
        auto tests_to_print = min(10uz, tests.size());
        outln("\nSlowest {} tests:", tests_to_print);

        quick_sort(tests, [&](auto const& lhs, auto const& rhs) {
            auto lhs_duration = lhs.end_time - lhs.start_time;
            auto rhs_duration = rhs.end_time - rhs.start_time;
            return lhs_duration > rhs_duration;
        });

        for (auto const& test : tests.span().trim(tests_to_print)) {
            auto name = LexicalPath::relative_path(test.input_path, app.test_root_path);
            auto duration = test.end_time - test.start_time;

            outln("{}: {}ms", name, duration.to_milliseconds());
        }
    }

    if (app.dump_gc_graph) {
        app.for_each_web_view([&](auto& view) {
            if (auto path = view.dump_gc_graph(); path.is_error())
                warnln("Failed to dump GC graph: {}", path.error());
            else
                outln("GC graph dumped to {}", path.value());
        });
    }

    app.destroy_web_views();

    if (all_tests_ok)
        return {};

    return Error::from_string_literal("Failed LibWeb tests");
}

}
