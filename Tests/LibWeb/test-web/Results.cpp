/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Results.h"
#include "Application.h"

#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Stream.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>

#if !defined(AK_OS_WINDOWS)
#    include <unistd.h>
#endif

namespace TestWeb {

static constexpr StringView MANIFEST_FILE_NAME = "results-manifest.js"sv;

static ErrorOr<ByteString> prepare_output_path(Test const& test)
{
    auto& app = Application::the();
    auto base_path = LexicalPath::join(app.results_directory, test.safe_relative_path);
    TRY(Core::Directory::create(base_path.dirname(), Core::Directory::CreateDirectories::Yes));
    return base_path.string();
}

static ErrorOr<void> initialize_manifest_array()
{
    auto path = LexicalPath::join(Application::the().results_directory, MANIFEST_FILE_NAME).string();
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted("window.RESULTS_MANIFEST = [\n];\n"sv));
    return {};
}

static ErrorOr<void> append_manifest_record(StringView json)
{
    auto path = LexicalPath::join(Application::the().results_directory, MANIFEST_FILE_NAME).string();
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::ReadWrite));
    TRY(file->seek(-3, SeekMode::FromEndPosition));
    auto line = ByteString::formatted("{},\n];\n", json);
    TRY(file->write_until_depleted(line.bytes()));
    return {};
}

static ErrorOr<void> copy_result_asset(StringView source_file_name, StringView destination_file_name)
{
    auto const& app = Application::the();
    auto source_path = LexicalPath::join(app.test_root_path, ByteString::formatted("test-web/{}", source_file_name)).string();
    auto destination_path = LexicalPath::join(app.results_directory, destination_file_name).string();
    return FileSystem::copy_file_or_directory(destination_path, source_path, FileSystem::RecursionMode::Disallowed, FileSystem::LinkMode::Disallowed, FileSystem::AddDuplicateFileMarker::No);
}

void append_result(Test const& test, TestResult result)
{
    StringBuilder builder;
    builder.appendff(
        "{{ \"type\": \"test\", \"name\": {}, \"relativePath\": {}, \"result\": {}, \"mode\": {}",
        JsonValue(test.safe_relative_path).serialized(),
        JsonValue(test.relative_path).serialized(),
        JsonValue(test_result_to_string(result)).serialized(),
        JsonValue(test_mode_to_string(test.mode)).serialized());

    auto base_path = LexicalPath::join(Application::the().results_directory, test.safe_relative_path).string();
    if (FileSystem::exists(ByteString::formatted("{}.logs.html", base_path)))
        builder.append(", \"hasLogs\": true"sv);

    if ((test.mode == TestMode::Ref || test.mode == TestMode::Screenshot) && test.diff_pixel_error_count > 0)
        builder.appendff(", \"pixelErrors\": {}, \"maxChannelDiff\": {}", test.diff_pixel_error_count, test.diff_maximum_error);

    builder.append(" }"sv);
    (void)append_manifest_record(builder.string_view());
}

ErrorOr<void> dump_screenshot_to_file(Gfx::Bitmap const& bitmap, StringView path)
{
    auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
    TRY(screenshot_file->write_until_depleted(encoded_data));
    return {};
}

ErrorOr<void> prepare_result_files(ReadonlySpan<Test> tests)
{
    auto& app = Application::the();

    TRY(copy_result_asset("results-index.html"sv, "index.html"sv));
    TRY(copy_result_asset("results-index.css"sv, "index.css"sv));

    TRY(initialize_manifest_array());

    auto run_header = ByteString::formatted(
        "{{ \"type\": \"run\", \"total\": {}, \"generatedAt\": {}, \"invocationCommandLine\": {}, \"showSkipped\": {} }}",
        tests.size(),
        UnixDateTime::now().seconds_since_epoch(),
        JsonValue(app.invocation_command_line).serialized(),
        app.verbosity >= Application::VERBOSITY_LEVEL_LOG_SKIPPED_TESTS ? "true" : "false");
    TRY(append_manifest_record(run_header));
    return append_manifest_record("{ \"type\": \"helper-logs\" }"sv);
}

ErrorOr<void> write_test_diff_to_results(Test const& test, ByteBuffer const& expectation)
{
    auto base_path = TRY(prepare_output_path(test));

    auto expected_path = ByteString::formatted("{}.expected.txt", base_path);
    auto expected_file = TRY(Core::File::open(expected_path, Core::File::OpenMode::Write));
    TRY(expected_file->write_until_depleted(expectation));

    auto actual_path = ByteString::formatted("{}.actual.txt", base_path);
    auto actual_file = TRY(Core::File::open(actual_path, Core::File::OpenMode::Write));
    TRY(actual_file->write_until_depleted(test.text.bytes()));

    auto diff_path = ByteString::formatted("{}.diff.txt", base_path);
    auto diff_file = TRY(Core::File::open(diff_path, Core::File::OpenMode::Write));

    auto hunks = TRY(Diff::from_text(expectation, test.text, 3));
    TRY(Diff::write_unified_header(test.expectation_path, test.expectation_path, *diff_file));
    for (auto const& hunk : hunks)
        TRY(Diff::write_unified(hunk, *diff_file, Diff::ColorOutput::No));

    auto html_path = ByteString::formatted("{}.diff.html", base_path);
    auto html_file = TRY(Core::File::open(html_path, Core::File::OpenMode::Write));

    TRY(html_file->write_until_depleted(R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
body { margin: 0; background: #0d1117; }
pre { margin: 0; padding: 16px; font-family: ui-monospace, monospace; font-size: 12px; line-height: 1.5; }
.add { background: #12261e; color: #3fb950; border-left: 3px solid #238636; padding-left: 8px; margin-left: -11px; }
.del { background: #2d1619; color: #f85149; border-left: 3px solid #f85149; padding-left: 8px; margin-left: -11px; }
.hunk { color: #58a6ff; font-weight: 500; }
.ctx { color: #8b949e; }
</style>
</head>
<body><pre>)html"sv));

    TRY(html_file->write_until_depleted("<span class=\"ctx\">"sv));
    TRY(html_file->write_formatted("--- {}\n", test.expectation_path));
    TRY(html_file->write_formatted("+++ {}\n", test.expectation_path));
    TRY(html_file->write_until_depleted("</span>"sv));

    for (auto const& hunk : hunks) {
        TRY(html_file->write_formatted("<span class=\"hunk\">{}</span>\n", hunk.location));

        for (auto const& line : hunk.lines) {
            auto escaped = escape_html_entities(line.content);
            switch (line.operation) {
            case Diff::Line::Operation::Addition:
                TRY(html_file->write_formatted("<span class=\"add\">+{}</span>\n", escaped));
                break;
            case Diff::Line::Operation::Removal:
                TRY(html_file->write_formatted("<span class=\"del\">-{}</span>\n", escaped));
                break;
            case Diff::Line::Operation::Context:
                TRY(html_file->write_formatted("<span class=\"ctx\"> {}</span>\n", escaped));
                break;
            }
        }
    }

    TRY(html_file->write_until_depleted("</pre></body></html>"sv));
    return {};
}

ErrorOr<void> write_screenshot_failure_results(Test& test, Gfx::Bitmap const& actual, Gfx::Bitmap const& expected)
{
    auto base_path = TRY(prepare_output_path(test));
    TRY(dump_screenshot_to_file(actual, ByteString::formatted("{}.actual.png", base_path)));
    TRY(dump_screenshot_to_file(expected, ByteString::formatted("{}.expected.png", base_path)));

    if (actual.width() == expected.width() && actual.height() == expected.height()) {
        auto diff = actual.diff(expected);
        test.diff_pixel_error_count = diff.pixel_error_count;
        test.diff_maximum_error = diff.maximum_error;

        auto diff_bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { actual.width(), actual.height() }));
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                auto pixel = actual.get_pixel(x, y);
                if (pixel != expected.get_pixel(x, y))
                    diff_bitmap->set_pixel(x, y, Gfx::Color(255, 0, 0));
                else
                    diff_bitmap->set_pixel(x, y, pixel.mixed_with(expected.get_pixel(x, y), 0.5f).mixed_with(Gfx::Color::White, 0.8f));
            }
        }
        TRY(dump_screenshot_to_file(*diff_bitmap, ByteString::formatted("{}.diff.png", base_path)));
    }

    return {};
}

}
