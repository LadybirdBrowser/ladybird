/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

static void populate_allowlist_for_url(Vector<ByteString>& allowlist, URL::URL const& url)
{
    if (url.scheme() == "file"sv) {
        auto canonical_path = LexicalPath::canonicalized_path(url.file_path());
        auto directory = LexicalPath::dirname(canonical_path);
        LexicalPath lexical_directory { directory };
        if (!directory.is_empty() && !lexical_directory.is_root()) {
            auto already_covered = any_of(allowlist, [&](auto const& allowed) {
                return lexical_directory.is_child_of(LexicalPath { allowed });
            });
            if (!already_covered)
                allowlist.append(directory);
        }
    } else {
        allowlist.clear();
    }
}

static bool is_file_request_allowed(Vector<ByteString> const& allowlist, StringView path)
{
    auto canonical = LexicalPath::canonicalized_path(path);

    if (!LexicalPath::is_absolute_path(canonical))
        return false;

    LexicalPath requested { canonical };
    return any_of(allowlist, [&](auto const& allowed_directory) {
        return requested.is_child_of(LexicalPath { allowed_directory });
    });
}

TEST_CASE(file_in_same_directory_is_allowed)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(is_file_request_allowed(allowlist, "/home/user/docs/style.css"sv));
    EXPECT(is_file_request_allowed(allowlist, "/home/user/docs/image.png"sv));
}

TEST_CASE(file_in_subdirectory_is_allowed)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(is_file_request_allowed(allowlist, "/home/user/docs/images/logo.png"sv));
    EXPECT(is_file_request_allowed(allowlist, "/home/user/docs/sub/deep/file.js"sv));
}

TEST_CASE(file_outside_directory_is_rejected)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(!is_file_request_allowed(allowlist, "/home/user/other/secret.txt"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/etc/passwd"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/home/user/.ssh/id_rsa"sv));
}

TEST_CASE(path_traversal_is_neutralized)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/../../../etc/passwd"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/../../.ssh/id_rsa"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/../other/secret.txt"sv));
}

TEST_CASE(sibling_directory_is_rejected)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/input/test.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/data/image.png"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/expected/output.txt"sv));
}

TEST_CASE(empty_allowlist_rejects_everything)
{
    Vector<ByteString> allowlist;

    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/page.html"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/tmp/file.txt"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/etc/passwd"sv));
}

TEST_CASE(non_file_navigation_clears_allowlist)
{
    Vector<ByteString> allowlist;
    auto file_url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, file_url);
    EXPECT(is_file_request_allowed(allowlist, "/home/user/docs/style.css"sv));

    auto http_url = URL::Parser::basic_parse("http://example.com"sv).release_value();
    populate_allowlist_for_url(allowlist, http_url);
    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs/style.css"sv));
    EXPECT(allowlist.is_empty());
}

TEST_CASE(root_directory_is_excluded)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/vmlinuz"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(allowlist.is_empty());
    EXPECT(!is_file_request_allowed(allowlist, "/etc/passwd"sv));
    EXPECT(!is_file_request_allowed(allowlist, "/vmlinuz"sv));
}

TEST_CASE(relative_path_is_rejected)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(!is_file_request_allowed(allowlist, "relative/path.txt"sv));
    EXPECT(!is_file_request_allowed(allowlist, "./local.txt"sv));
    EXPECT(!is_file_request_allowed(allowlist, ""sv));
}

TEST_CASE(multiple_file_navigations_accumulate)
{
    Vector<ByteString> allowlist;
    auto url1 = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url1);

    auto url2 = URL::create_with_file_scheme("/home/user/images/gallery.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url2);

    EXPECT(is_file_request_allowed(allowlist, "/home/user/docs/style.css"sv));
    EXPECT(is_file_request_allowed(allowlist, "/home/user/images/photo.jpg"sv));
    EXPECT_EQ(allowlist.size(), 2u);
}

TEST_CASE(duplicate_directory_is_not_added)
{
    Vector<ByteString> allowlist;
    auto url1 = URL::create_with_file_scheme("/home/user/docs/page1.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url1);

    auto url2 = URL::create_with_file_scheme("/home/user/docs/page2.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url2);

    EXPECT_EQ(allowlist.size(), 1u);
}

TEST_CASE(child_directory_is_not_added_when_parent_exists)
{
    Vector<ByteString> allowlist;
    auto url1 = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url1);

    auto url2 = URL::create_with_file_scheme("/home/user/docs/sub/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url2);

    EXPECT_EQ(allowlist.size(), 1u);
}

TEST_CASE(prefix_confusion_is_prevented)
{
    Vector<ByteString> allowlist;
    auto url = URL::create_with_file_scheme("/home/user/docs/page.html"sv).release_value();
    populate_allowlist_for_url(allowlist, url);

    EXPECT(!is_file_request_allowed(allowlist, "/home/user/docs-secret/passwords.txt"sv));
}
