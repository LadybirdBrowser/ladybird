/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Profile.h>

static ByteString test_root()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-profile-{}", Core::System::getpid())).string();
}

static WebView::ProfileRoots roots()
{
    auto root = test_root();
    return {
        .config = LexicalPath::join(root, "config-root"sv).string(),
        .data = LexicalPath::join(root, "data-root"sv).string(),
        .cache = LexicalPath::join(root, "cache-root"sv).string(),
        .runtime = LexicalPath::join(root, "runtime-root"sv).string(),
        .temporary = LexicalPath::join(root, "temporary-root"sv).string(),
    };
}

static void remove_test_root()
{
    if (FileSystem::exists(test_root()))
        MUST(FileSystem::remove(test_root(), FileSystem::RecursionMode::Allowed));
}

TEST_CASE(profile_names)
{
    EXPECT(WebView::Profile::is_valid_name("default"sv));
    EXPECT(WebView::Profile::is_valid_name("release-2_0"sv));
    EXPECT(WebView::Profile::is_valid_name("legacy"sv));
    EXPECT(!WebView::Profile::is_valid_name({}));
    EXPECT(!WebView::Profile::is_valid_name("Default"sv));
    EXPECT(!WebView::Profile::is_valid_name("has.dot"sv));
    EXPECT(!WebView::Profile::is_valid_name("has/slash"sv));
}

TEST_CASE(routing_identity)
{
    EXPECT_EQ(WebView::Profile::routing_identifier("/profiles/default"sv), WebView::Profile::routing_identifier("/profiles/default"sv));
    EXPECT_NE(WebView::Profile::routing_identifier("/profiles/default"sv), WebView::Profile::routing_identifier("/profiles/work"sv));
    EXPECT_EQ(WebView::Profile::routing_identifier("/profiles/default"sv).length(), 32u);
}

TEST_CASE(named_and_default_layouts)
{
    remove_test_root();
    auto profile_roots = roots();

    auto named = MUST(WebView::Profile::create({ .name = "work" }, profile_roots));
    EXPECT_EQ(named.identifier(), "work"sv);
    EXPECT_EQ(named.paths().config, LexicalPath::join(profile_roots.config, "Ladybird/Profiles/work"sv).string());
    EXPECT_EQ(named.paths().data, LexicalPath::join(profile_roots.data, "Ladybird/Profiles/work"sv).string());
    EXPECT_EQ(named.paths().cache, LexicalPath::join(profile_roots.cache, "Ladybird/Profiles/work"sv).string());
    EXPECT_EQ(named.paths().runtime, LexicalPath::join(profile_roots.runtime, "Ladybird/Profiles/work"sv).string());

    auto default_profile = MUST(WebView::Profile::create({}, profile_roots));
    EXPECT_EQ(default_profile.identifier(), "default"sv);
    EXPECT_EQ(default_profile.paths().config, LexicalPath::join(profile_roots.config, "Ladybird/Profiles/default"sv).string());
    EXPECT_EQ(default_profile.paths().data, LexicalPath::join(profile_roots.data, "Ladybird/Profiles/default"sv).string());
    EXPECT_EQ(default_profile.paths().cache, LexicalPath::join(profile_roots.cache, "Ladybird/Profiles/default"sv).string());
    EXPECT_EQ(default_profile.paths().runtime, LexicalPath::join(profile_roots.runtime, "Ladybird/Profiles/default"sv).string());

    auto explicit_default_profile = MUST(WebView::Profile::create({ .name = "default" }, profile_roots));
    EXPECT_EQ(explicit_default_profile.paths().identity, default_profile.paths().identity);
    remove_test_root();
}

TEST_CASE(custom_layout)
{
    remove_test_root();
    auto root = LexicalPath::join(test_root(), "custom"sv).string();
    auto profile = MUST(WebView::Profile::create({ .path = root }, roots()));
    auto canonical_root = MUST(FileSystem::real_path(root));
    EXPECT_EQ(profile.paths().config, LexicalPath::join(canonical_root, "config"sv).string());
    EXPECT_EQ(profile.paths().data, LexicalPath::join(canonical_root, "data"sv).string());
    EXPECT_EQ(profile.paths().cache, LexicalPath::join(canonical_root, "cache"sv).string());
    EXPECT_EQ(profile.paths().runtime, LexicalPath::join(canonical_root, "runtime"sv).string());
    EXPECT_EQ(profile.paths().identity, canonical_root);
    remove_test_root();
}

TEST_CASE(invalid_selection)
{
    remove_test_root();
    EXPECT(WebView::Profile::create({ .name = "UPPER" }, roots()).is_error());
    EXPECT(WebView::Profile::create({ .path = "relative/path" }, roots()).is_error());
    EXPECT(WebView::Profile::create({ .name = "one", .temporary = true }, roots()).is_error());
    EXPECT(WebView::Profile::create({ .name = "one", .path = "/absolute" }, roots()).is_error());
    remove_test_root();
}

TEST_CASE(temporary_profiles_are_unique_and_removed)
{
    remove_test_root();
    ByteString first_root;
    ByteString second_root;
    {
        auto first = MUST(WebView::Profile::create({ .temporary = true }, roots()));
        auto second = MUST(WebView::Profile::create({ .temporary = true }, roots()));
        first_root = first.paths().identity;
        second_root = second.paths().identity;
        EXPECT_NE(first_root, second_root);
        EXPECT(FileSystem::is_directory(first.paths().config));
        EXPECT(FileSystem::is_directory(second.paths().data));
    }
    EXPECT(!FileSystem::exists(first_root));
    EXPECT(!FileSystem::exists(second_root));
    remove_test_root();
}
