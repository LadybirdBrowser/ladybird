/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Results.h"
#include "Application.h"
#include "Display.h"

#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>

namespace TestWeb {

static ErrorOr<ByteString> prepare_output_path(Test const& test)
{
    auto& app = Application::the();
    auto base_path = LexicalPath::join(app.results_directory, test.safe_relative_path);
    TRY(Core::Directory::create(base_path.dirname(), Core::Directory::CreateDirectories::Yes));
    return base_path.string();
}

ErrorOr<void> dump_screenshot_to_file(Gfx::Bitmap const& bitmap, StringView path)
{
    auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
    TRY(screenshot_file->write_until_depleted(encoded_data));
    return {};
}

ErrorOr<void> generate_result_files(ReadonlySpan<Test> tests, ReadonlySpan<TestCompletion> non_passing_tests)
{
    auto& app = Application::the();
    auto& display = Display::the();

    bool const has_helper_logs = FileSystem::exists(LexicalPath::join(app.results_directory, "helper-process-logs.html"sv).string());
    auto const generated_at = UnixDateTime::now();

    StringBuilder js;
    js.append("const RESULTS_DATA = {\n"sv);
    js.appendff("  \"summary\": {{ \"total\": {}, \"fail\": {}, \"timeout\": {}, \"crashed\": {}, \"skipped\": {} }},\n",
        total_tests(),
        display.fail_count,
        display.timeout_count,
        display.crashed_count,
        display.skipped_count);
    js.appendff("  \"generatedAt\": {},\n", generated_at.seconds_since_epoch());
    js.appendff("  \"invocationCommandLine\": {},\n", JsonValue(app.invocation_command_line).serialized());
    js.appendff("  \"hasLogs\": {},\n", has_helper_logs ? "true" : "false");
    js.append("  \"tests\": [\n"sv);

    bool first = true;
    for (auto const& result : non_passing_tests) {
        if (result.result == TestResult::Skipped && app.verbosity < Application::VERBOSITY_LEVEL_LOG_SKIPPED_TESTS)
            continue;

        if (!first)
            js.append(",\n"sv);
        first = false;

        auto const& test = tests[result.test_index];
        auto base_path = TRY(prepare_output_path(test));
        bool has_std_logs = FileSystem::exists(ByteString::formatted("{}.logs.html", base_path));

        js.appendff("    {{ \"name\": \"{}\", \"result\": \"{}\", \"mode\": \"{}\", \"hasLogs\": {}",
            test.safe_relative_path,
            test_result_to_string(result.result),
            test_mode_to_string(test.mode),
            has_std_logs ? "true" : "false");
        if ((test.mode == TestMode::Ref || test.mode == TestMode::Screenshot) && test.diff_pixel_error_count > 0)
            js.appendff(", \"pixelErrors\": {}, \"maxChannelDiff\": {}", test.diff_pixel_error_count, test.diff_maximum_error);
        js.append(" }"sv);
    }

    js.append("\n  ]\n};\n"sv);

    auto js_path = LexicalPath::join(app.results_directory, "results.js"sv).string();
    auto js_file = TRY(Core::File::open(js_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(js_file->write_until_depleted(js.string_view().bytes()));

    auto source_html_path = LexicalPath::join(app.test_root_path, "test-web/results-index.html"sv).string();
    auto dest_html_path = LexicalPath::join(app.results_directory, "index.html"sv).string();
    auto source_html = TRY(Core::File::open(source_html_path, Core::File::OpenMode::Read));
    auto html_contents = TRY(source_html->read_until_eof());
    auto dest_html = TRY(Core::File::open(dest_html_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(dest_html->write_until_depleted(html_contents));

    auto source_css_path = LexicalPath::join(app.test_root_path, "test-web/results-index.css"sv).string();
    auto dest_css_path = LexicalPath::join(app.results_directory, "index.css"sv).string();
    auto source_css = TRY(Core::File::open(source_css_path, Core::File::OpenMode::Read));
    auto css_contents = TRY(source_css->read_until_eof());
    auto dest_css = TRY(Core::File::open(dest_css_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(dest_css->write_until_depleted(css_contents));

    return {};
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
