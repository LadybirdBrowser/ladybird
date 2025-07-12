/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/MimeData.h>
#include <LibTest/TestCase.h>

static void check_filename_mimetype(Vector<StringView> const& filepaths, StringView expected_mime_type)
{
    for (auto const& filename : filepaths) {
        dbgln(filename, "\n");
        auto const& guessed_mime_type = Core::guess_mime_type_based_on_filename(filename);
        EXPECT_EQ(guessed_mime_type, expected_mime_type);
    }
}

auto text_plain_filenames = Vector {
    "main.c"sv,
    "hello.txt"sv,
    ".history"sv,
    ".shellrc"sv,
    "CMakeList.txt"sv,
};
auto html_filenames = Vector {
    "about.html"sv,
    "send-data-blob.htm"sv,
    "dir/settings.html"sv,
};
auto xhtml_filenames = Vector {
    "about.xhtml"sv,
    "content.xht"sv,
};
auto gzip_filenames = Vector {
    "download.iso.gz"sv,
    "backup.gzip"sv,
    "hello.html.gz"sv,
};
auto markdown_filenames = Vector {
    "README.md"sv,
    "changelog.md"sv,
};
auto shell_filenames = Vector {
    "script.sh"sv,
};

TEST_CASE(various_types_guessed)
{
    check_filename_mimetype(text_plain_filenames, "text/plain"sv);
    check_filename_mimetype(html_filenames, "text/html"sv);
    check_filename_mimetype(xhtml_filenames, "application/xhtml+xml"sv);
    check_filename_mimetype(gzip_filenames, "application/gzip"sv);
    check_filename_mimetype(markdown_filenames, "text/markdown"sv);
    check_filename_mimetype(shell_filenames, "text/x-shellscript"sv);
}
