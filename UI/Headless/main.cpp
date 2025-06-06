/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibWebView/Utilities.h>
#include <UI/Headless/Application.h>
#include <UI/Headless/Test.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    WebView::platform_init();

    auto app = Ladybird::Application::create(arguments);
    TRY(app->launch_services());

    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(app->resources_folder))));

    auto theme_path = LexicalPath::join(app->resources_folder, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto const& browser_options = Ladybird::Application::browser_options();
    Web::DevicePixelSize window_size { browser_options.window_width, browser_options.window_height };

    VERIFY(!app->test_root_path.is_empty());

    app->test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);
    TRY(app->launch_test_fixtures());

    return Ladybird::run_tests(theme, window_size);
}
