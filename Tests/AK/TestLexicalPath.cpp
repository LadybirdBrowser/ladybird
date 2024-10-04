/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <max.wipfli@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>

TEST_CASE(relative_path)
{
    EXPECT_EQ(LexicalPath::relative_path("/tmp/abc.txt"sv, "/tmp"sv), "abc.txt"sv);
    EXPECT_EQ(LexicalPath::relative_path("/tmp/abc.txt"sv, "/tmp/"sv), "abc.txt"sv);
    EXPECT_EQ(LexicalPath::relative_path("/tmp/abc.txt"sv, "/"sv), "tmp/abc.txt"sv);
    EXPECT_EQ(LexicalPath::relative_path("/tmp/abc.txt"sv, "/usr"sv), "../tmp/abc.txt"sv);

    EXPECT_EQ(LexicalPath::relative_path("/tmp/foo.txt"sv, "tmp"sv), ""sv);
    EXPECT_EQ(LexicalPath::relative_path("tmp/foo.txt"sv, "/tmp"sv), ""sv);

    EXPECT_EQ(LexicalPath::relative_path("/tmp/foo/bar/baz.txt"sv, "/tmp/bar/foo/"sv), "../../foo/bar/baz.txt"sv);
}

TEST_CASE(regular_absolute_path)
{
    LexicalPath path("/home/anon/foo.txt");
    EXPECT_EQ(path.string(), "/home/anon/foo.txt");
    EXPECT_EQ(path.dirname(), "/home/anon");
    EXPECT_EQ(path.basename(), "foo.txt");
    EXPECT_EQ(path.title(), "foo");
    EXPECT_EQ(path.extension(), "txt");
    EXPECT(path.has_extension(".txt"sv));
    EXPECT(path.has_extension("txt"sv));
    EXPECT(!path.has_extension("txxt"sv));
    EXPECT_EQ(path.parts_view().size(), 3u);
    EXPECT_EQ(path.parts_view()[0], "home");
    EXPECT_EQ(path.parts_view()[1], "anon");
    EXPECT_EQ(path.parts_view()[2], "foo.txt");
}

TEST_CASE(regular_relative_path)
{
    LexicalPath path("anon/foo.txt");
    EXPECT_EQ(path.dirname(), "anon");
    EXPECT_EQ(path.basename(), "foo.txt");
    EXPECT_EQ(path.parts_view().size(), 2u);
    EXPECT_EQ(path.parts_view()[0], "anon");
    EXPECT_EQ(path.parts_view()[1], "foo.txt");
}

TEST_CASE(single_dot)
{
    {
        LexicalPath path("/home/./anon/foo.txt");
        EXPECT_EQ(path.string(), "/home/anon/foo.txt");
    }
    {
        LexicalPath path("./test.txt");
        EXPECT_EQ(path.string(), "test.txt");
    }
    {
        LexicalPath path("./../test.txt");
        EXPECT_EQ(path.string(), "../test.txt");
    }
}

TEST_CASE(relative_path_with_dotdot)
{
    LexicalPath path("anon/../../foo.txt");
    EXPECT_EQ(path.string(), "../foo.txt");
    EXPECT_EQ(path.dirname(), "..");
    EXPECT_EQ(path.basename(), "foo.txt");
    EXPECT_EQ(path.parts_view().size(), 2u);
    EXPECT_EQ(path.parts_view()[0], "..");
    EXPECT_EQ(path.parts_view()[1], "foo.txt");
}

TEST_CASE(absolute_path_with_dotdot)
{
    {
        LexicalPath path("/test/../foo.txt");
        EXPECT_EQ(path.string(), "/foo.txt");
    }
    {
        LexicalPath path("/../../foo.txt");
        EXPECT_EQ(path.string(), "/foo.txt");
    }
}

TEST_CASE(more_dotdot_paths)
{
    EXPECT_EQ(LexicalPath::canonicalized_path("/home/user/../../not/home"), "/not/home");
    EXPECT_EQ(LexicalPath::canonicalized_path("/../../../../"), "/");
    EXPECT_EQ(LexicalPath::canonicalized_path("./../../../../"), "../../../..");
    EXPECT_EQ(LexicalPath::canonicalized_path("../../../../../"), "../../../../..");
}

TEST_CASE(the_root_path)
{
    LexicalPath path("/");
    EXPECT_EQ(path.dirname(), "/");
    EXPECT_EQ(path.basename(), "/");
    EXPECT_EQ(path.title(), "/");
    EXPECT_EQ(path.parts_view().size(), 0u);
}

TEST_CASE(the_dot_path)
{
    LexicalPath path(".");
    EXPECT_EQ(path.string(), ".");
    EXPECT_EQ(path.dirname(), ".");
    EXPECT_EQ(path.basename(), ".");
    EXPECT_EQ(path.title(), ".");
}

TEST_CASE(double_slash)
{
    LexicalPath path("//home/anon/foo.txt");
    EXPECT_EQ(path.string(), "/home/anon/foo.txt");
}

TEST_CASE(trailing_slash)
{
    LexicalPath path("/home/anon/");
    EXPECT_EQ(path.string(), "/home/anon");
    EXPECT_EQ(path.dirname(), "/home");
    EXPECT_EQ(path.basename(), "anon");
    EXPECT_EQ(path.parts_view().size(), 2u);
}

TEST_CASE(resolve_absolute_path)
{
    EXPECT_EQ(LexicalPath::absolute_path("/home/anon", "foo.txt"), "/home/anon/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("/home/anon/", "foo.txt"), "/home/anon/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("/home/anon", "././foo.txt"), "/home/anon/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("/home/anon/quux", "../foo.txt"), "/home/anon/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("/home/anon/quux", "../test/foo.txt"), "/home/anon/test/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("quux", "../test/foo.txt"), "test/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("quux", "../../test/foo.txt"), "../test/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("quux/bar", "../../test/foo.txt"), "test/foo.txt");
    EXPECT_EQ(LexicalPath::absolute_path("quux/bar/", "../../test/foo.txt"), "test/foo.txt");
}

TEST_CASE(has_extension)
{
    {
        LexicalPath path("/tmp/simple.png");
        EXPECT(path.has_extension(".png"sv));
        EXPECT(path.has_extension(".pnG"sv));
        EXPECT(path.has_extension(".PNG"sv));
    }

    {
        LexicalPath path("/TMP/SIMPLE.PNG");
        EXPECT(path.has_extension(".png"sv));
        EXPECT(path.has_extension(".pnG"sv));
        EXPECT(path.has_extension(".PNG"sv));
    }

    {
        LexicalPath path(".png");
        EXPECT(path.has_extension(".png"sv));
    }

    {
        LexicalPath path("png");
        EXPECT_EQ(path.has_extension(".png"sv), false);
    }
}

TEST_CASE(join)
{
    EXPECT_EQ(LexicalPath::join("anon"sv, "foo.txt"sv).string(), "anon/foo.txt"sv);
    EXPECT_EQ(LexicalPath::join("/home"sv, "anon/foo.txt"sv).string(), "/home/anon/foo.txt"sv);
    EXPECT_EQ(LexicalPath::join("/"sv, "foo.txt"sv).string(), "/foo.txt"sv);
    EXPECT_EQ(LexicalPath::join("/home"sv, "anon"sv, "foo.txt"sv).string(), "/home/anon/foo.txt"sv);
}

TEST_CASE(append)
{
    LexicalPath path("/home/anon/");
    auto new_path = path.append("foo.txt"sv);
    EXPECT_EQ(new_path.string(), "/home/anon/foo.txt");
}

TEST_CASE(parent)
{
    {
        LexicalPath path("/home/anon/foo.txt");
        auto parent = path.parent();
        EXPECT_EQ(parent.string(), "/home/anon");
    }
    {
        LexicalPath path("anon/foo.txt");
        auto parent = path.parent();
        EXPECT_EQ(parent.string(), "anon");
    }
    {
        LexicalPath path("foo.txt");
        auto parent = path.parent();
        EXPECT_EQ(parent.string(), ".");
        auto parent_of_parent = parent.parent();
        EXPECT_EQ(parent_of_parent.string(), "..");
    }
    {
        LexicalPath path("/");
        auto parent = path.parent();
        EXPECT_EQ(parent.string(), "/");
    }
}

TEST_CASE(is_child_of)
{
    {
        LexicalPath parent("/a/parent/directory");
        LexicalPath child("/a/parent/directory/a/child");
        LexicalPath mismatching("/not/a/child/directory");
        EXPECT(child.is_child_of(parent));
        EXPECT(child.is_child_of(child));
        EXPECT(parent.is_child_of(parent));
        EXPECT(!parent.is_child_of(child));
        EXPECT(!mismatching.is_child_of(parent));

        EXPECT(parent.is_child_of(parent.parent()));
        EXPECT(child.parent().parent().is_child_of(parent));
        EXPECT(!child.parent().parent().parent().is_child_of(parent));
    }
    {
        LexicalPath root("/");
        EXPECT(LexicalPath("/").is_child_of(root));
        EXPECT(LexicalPath("/any").is_child_of(root));
        EXPECT(LexicalPath("/child/directory").is_child_of(root));
    }
    {
        LexicalPath relative("folder");
        LexicalPath relative_child("folder/sub");
        LexicalPath absolute("/folder");
        LexicalPath absolute_child("/folder/sub");
        EXPECT(relative_child.is_child_of(relative));
        EXPECT(absolute_child.is_child_of(absolute));

        EXPECT(relative.is_child_of(absolute));
        EXPECT(relative.is_child_of(absolute_child));
        EXPECT(relative_child.is_child_of(absolute));
        EXPECT(relative_child.is_child_of(absolute_child));

        EXPECT(!absolute.is_child_of(relative));
        EXPECT(!absolute_child.is_child_of(relative));
        EXPECT(!absolute_child.is_child_of(relative_child));
    }
}
