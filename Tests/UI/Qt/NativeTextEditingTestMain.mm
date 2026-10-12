/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestSuite.h>

#import <AppKit/AppKit.h>

int main(int argc, char** argv)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        Vector<StringView> arguments;
        for (int i = 0; i < argc; ++i)
            arguments.append({ argv[i], strlen(argv[i]) });
        auto result = Test::TestSuite::the().main(argv[0], arguments);
        Test::TestSuite::release();
        return result != 0;
    }
}
