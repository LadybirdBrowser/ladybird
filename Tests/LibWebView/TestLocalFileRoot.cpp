/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebView/LocalFileRoot.h>

using Scope = WebView::LocalFileRoot::Scope;

namespace {

// A home directory holding a profile's cache, which a symbolic link reaches, and folders of the user's own.
class TestHome {
public:
    TestHome()
        : m_home(MUST(FileSystem::real_path(m_directory.path().string())))
    {
        MUST(Core::Directory::create(join("Documents/Project"sv), Core::Directory::CreateDirectories::Yes));
        MUST(Core::Directory::create(join("Storage/Cache/Ladybird"sv), Core::Directory::CreateDirectories::Yes));
        MUST(Core::System::symlink(join("Storage/Cache"sv), join(".cache"sv)));
    }

    ByteString join(StringView path) const { return LexicalPath::join(m_home, path).string(); }

    WebView::LocalFileGrantPolicy policy(Optional<ByteString> root_for_tests = {}) const
    {
        return { m_home, { join(".cache/Ladybird"sv) }, move(root_for_tests) };
    }

private:
    Test::TemporaryDirectory m_directory { "TestLocalFileRoot"sv };
    ByteString m_home;
};

}

TEST_CASE(roots_cover_what_is_under_them)
{
    WebView::LocalFileRoot directory { "/a/b"sv, Scope::Directory };
    EXPECT(directory.covers("/a/b"sv));
    EXPECT(directory.covers("/a/b/c.html"sv));
    EXPECT(!directory.covers("/a/bc/d.html"sv));
    EXPECT(!directory.covers("/a"sv));

    WebView::LocalFileRoot file { "/a/b/c.html"sv, Scope::File };
    EXPECT(file.covers("/a/b/c.html"sv));
    EXPECT(!file.covers("/a/b/d.html"sv));
    EXPECT(!file.covers("/a/b"sv));
}

TEST_CASE(a_file_the_user_opens_grants_its_folder)
{
    TestHome home;
    auto policy = home.policy();

    auto file = home.join("Documents/Project/index.html"sv);
    EXPECT(policy.root_for_opened_file(file) == WebView::LocalFileRoot { home.join("Documents/Project"sv), Scope::Directory });
    EXPECT(policy.root_for_opened_file("/srv/site/index.html"sv) == WebView::LocalFileRoot { "/srv/site"sv, Scope::Directory });
}

TEST_CASE(a_folder_that_is_too_wide_grants_only_the_file)
{
    TestHome home;
    auto policy = home.policy();

    // Home, and anything above it.
    auto in_home = home.join("page.html"sv);
    EXPECT(policy.root_for_opened_file(in_home) == WebView::LocalFileRoot { in_home, Scope::File });
    EXPECT(policy.root_for_opened_file("/page.html"sv) == WebView::LocalFileRoot { "/page.html"sv, Scope::File });

    // A folder holding the profile's directory, as named through a symbolic link.
    auto beside_profile = home.join("Storage/Cache/page.html"sv);
    EXPECT(policy.root_for_opened_file(beside_profile) == WebView::LocalFileRoot { beside_profile, Scope::File });

    // The profile's directory itself: the file is all it grants, and nothing beside it.
    auto in_profile = home.join("Storage/Cache/Ladybird/page.html"sv);
    auto root = policy.root_for_opened_file(in_profile);
    EXPECT(root == WebView::LocalFileRoot { in_profile, Scope::File });
    EXPECT(!root.covers(home.join("Storage/Cache/Ladybird/Cookies.db"sv)));
}

TEST_CASE(a_test_root_is_granted_whole)
{
    TestHome home;
    auto policy = home.policy(home.join("Documents"sv));

    EXPECT(policy.root_for_opened_file(home.join("Documents/Project/index.html"sv)) == WebView::LocalFileRoot { home.join("Documents"sv), Scope::Directory });
    EXPECT(policy.root_for_opened_file("/srv/site/index.html"sv) == WebView::LocalFileRoot { "/srv/site"sv, Scope::Directory });
}

TEST_CASE(a_saved_root_comes_back_only_if_still_allowed)
{
    TestHome home;
    auto policy = home.policy();

    WebView::LocalFileRoot folder { home.join("Documents/Project"sv), Scope::Directory };
    EXPECT(policy.restore(folder) == folder);

    // A file root comes back as it was, whatever is at its path now.
    WebView::LocalFileRoot file { home.join("Documents/Project"sv), Scope::File };
    EXPECT(policy.restore(file) == file);

    // A folder that's no longer allowed is dropped, not narrowed to a file root that would cover nothing in it.
    EXPECT(!policy.restore({ home.join("Storage"sv), Scope::Directory }).has_value());
    EXPECT(!policy.restore({ home.join(""sv), Scope::Directory }).has_value());
}
