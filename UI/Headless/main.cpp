/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Promise.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/Headless/Application.h>
#include <UI/Headless/HeadlessWebView.h>
#include <UI/Headless/Test.h>

static ErrorOr<NonnullRefPtr<Core::Timer>> load_page_for_screenshot_and_exit(Core::EventLoop& event_loop, Ladybird::HeadlessWebView& view, URL::URL const& url, int screenshot_timeout)
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

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    WebView::platform_init();

    auto app = Ladybird::Application::create(arguments);
    TRY(app->launch_services());

    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(app->resources_folder))));

    auto theme_path = LexicalPath::join(app->resources_folder, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    static Web::DevicePixelSize window_size { app->width, app->height };

    if (!app->test_root_path.is_empty()) {
        app->test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);
        TRY(app->launch_test_fixtures());
        TRY(Ladybird::run_tests(theme, window_size));

        return 0;
    }

    auto& view = app->create_web_view(move(theme), window_size);

    VERIFY(!WebView::Application::browser_options().urls.is_empty());
    auto const& url = WebView::Application::browser_options().urls.first();
    if (!url.is_valid()) {
        warnln("Invalid URL: \"{}\"", url);
        return Error::from_string_literal("Invalid URL");
    }

    if (app->dump_layout_tree || app->dump_text) {
        Ladybird::Test test { app->dump_layout_tree ? Ladybird::TestMode::Layout : Ladybird::TestMode::Text };
        Ladybird::run_dump_test(view, test, url, app->per_test_timeout_in_seconds * 1000);

        auto completion = MUST(view.test_promise().await());
        return completion.result == Ladybird::TestResult::Pass ? 0 : 1;
    }

    RefPtr<Core::Timer> timer;
    if (!WebView::Application::browser_options().webdriver_content_ipc_path.has_value())
        timer = TRY(load_page_for_screenshot_and_exit(Core::EventLoop::current(), view, url, app->screenshot_timeout));

    return app->execute();
}
