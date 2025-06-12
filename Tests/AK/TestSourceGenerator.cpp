/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/SourceGenerator.h>

TEST_CASE(wrap_builder)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append("Hello, World!");

    EXPECT_EQ(builder.string_view(), "Hello, World!");
}

TEST_CASE(generate_c_code)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.set("name", "foo");

    generator.append("const char* @name@ (void) { return \"@name@\"; }");

    EXPECT_EQ(generator.as_string_view(), "const char* foo (void) { return \"foo\"; }");
}

TEST_CASE(scoped)
{
    StringBuilder builder;
    SourceGenerator global_generator { builder };

    global_generator.append("\n");

    global_generator.set("foo", "foo-0");
    global_generator.set("bar", "bar-0");
    global_generator.append("@foo@ @bar@\n"); // foo-0 bar-0

    {
        auto scoped_generator_1 = global_generator.fork();
        scoped_generator_1.set("bar", "bar-1");
        global_generator.append("@foo@ @bar@\n"); // foo-0 bar-0
    }

    global_generator.append("@foo@ @bar@\n"); // foo-0 bar-0

    {
        auto scoped_generator_2 = global_generator.fork();
        scoped_generator_2.set("foo", "foo-2");
        scoped_generator_2.append("@foo@ @bar@\n"); // foo-2 bar-0

        {
            auto scoped_generator_3 = scoped_generator_2.fork();
            scoped_generator_3.set("bar", "bar-3");
            scoped_generator_3.append("@foo@ @bar@\n"); // foo-2 bar-3
        }

        {
            auto scoped_generator_4 = global_generator.fork();
            scoped_generator_4.append("@foo@ @bar@\n"); // foo-0 bar-0
        }

        scoped_generator_2.append("@foo@ @bar@\n"); // foo-2 bar-0
    }

    EXPECT_EQ(global_generator.as_string_view(), "\nfoo-0 bar-0\nfoo-0 bar-0\nfoo-0 bar-0\nfoo-2 bar-0\nfoo-2 bar-3\nfoo-0 bar-0\nfoo-2 bar-0\n");
}
