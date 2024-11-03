/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibTest/TestSuite.h>

#define TEST_MAIN main

int TEST_MAIN(int argc, char** argv)
{
    if (argc < 1 || !argv[0] || '\0' == *argv[0]) {
        warnln("Test main does not have a valid test name!");
        return 1;
    }

    Vector<StringView> arguments;
    arguments.ensure_capacity(argc);
    for (auto i = 0; i < argc; ++i)
        arguments.append({ argv[i], strlen(argv[i]) });

    int ret = ::Test::TestSuite::the().main(argv[0], arguments);
    ::Test::TestSuite::release();
    // As TestSuite::main() returns the number of test cases that did not pass,
    // ret can be >=256 which cannot be returned as an exit status directly.
    // Return 0 if all of the test cases pass and return 1 otherwise.
    return ret != 0;
}
