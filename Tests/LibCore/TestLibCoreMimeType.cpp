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
    "main.c"_sv,
    "hello.txt"_sv,
    ".history"_sv,
    ".shellrc"_sv,
    "CMakeList.txt"_sv,
};
// FIXME: fails because .xht extension is in MimeType text/html and application/xhtml+xml
// auto html_filenames = Vector {"about.html"_sv, "send-data-blob.htm"_sv, "content.xht"_sv, "dir/settings.html"_sv,};
auto xhtml_filenames = Vector {
    "about.xhtml"_sv,
    "content.xht"_sv,
};
auto gzip_filenames = Vector {
    "download.iso.gz"_sv,
    "backup.gzip"_sv,
    "hello.html.gz"_sv,
};
auto markdown_filenames = Vector {
    "README.md"_sv,
    "changelog.md"_sv,
};
auto shell_filenames = Vector {
    "script.sh"_sv,
};

TEST_CASE(various_types_guessed)
{
    check_filename_mimetype(text_plain_filenames, "text/plain"_sv);
    // FIXME: fails because .xht extension is in MimeType text/html and application/xhtml+xml
    // check_filename_mimetype(html_filenames, "text/html"_sv);
    check_filename_mimetype(xhtml_filenames, "application/xhtml+xml"_sv);
    check_filename_mimetype(gzip_filenames, "application/gzip"_sv);
    check_filename_mimetype(markdown_filenames, "text/markdown"_sv);
    check_filename_mimetype(shell_filenames, "text/x-shellscript"_sv);
}
