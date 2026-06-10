/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Collection.h"
#include "Application.h"

#include <LibCore/DirIterator.h>
#include <LibFileSystem/FileSystem.h>

namespace TestWeb {

bool is_valid_test_name(StringView test_name)
{
    auto valid_test_file_suffixes = { ".htm"sv, ".html"sv, ".svg"sv, ".xhtml"sv, ".xht"sv, ".pdf"sv };
    return any_of(valid_test_file_suffixes, [&](auto suffix) { return test_name.ends_with(suffix); });
}

ErrorOr<void> collect_crash_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_crash_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }
        if (!is_valid_test_name(name))
            continue;

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Crash, input_path, {}, relative_path, relative_path });
    }

    return {};
}

ErrorOr<void> collect_dump_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail, TestMode mode)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);

    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_dump_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name), mode));
            continue;
        }

        if (!is_valid_test_name(name))
            continue;

        auto expectation_path = ByteString::formatted("{}/expected/{}/{}.txt", path, trail, LexicalPath::title(name));
        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ mode, input_path, move(expectation_path), relative_path, relative_path });
    }

    return {};
}

ErrorOr<void> collect_ref_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_ref_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }

        if (!is_valid_test_name(name))
            continue;

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Ref, input_path, {}, relative_path, relative_path });
    }

    return {};
}

static StringView screenshot_platform_name()
{
#if defined(AK_OS_MACOS)
    return "macos"sv;
#elif defined(AK_OS_LINUX)
    return "linux"sv;
#elif defined(AK_OS_WINDOWS)
    return "windows"sv;
#else
#    error "Unhandled platform for screenshot expectations"
#endif
}

static ByteString screenshot_expectation_path(StringView path, StringView trail, StringView name)
{
    auto title = LexicalPath::title(name);
    auto platform_expectation_path = ByteString::formatted("{}/expected-{}/{}/{}.png", path, screenshot_platform_name(), trail, title);
    if (FileSystem::exists(platform_expectation_path))
        return platform_expectation_path;
    return ByteString::formatted("{}/expected/{}/{}.png", path, trail, title);
}

ErrorOr<void> collect_screenshot_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_screenshot_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }

        if (!is_valid_test_name(name))
            continue;

        auto expectation_path = screenshot_expectation_path(path, trail, name);
        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Screenshot, input_path, move(expectation_path), relative_path, relative_path });
    }

    return {};
}

}
