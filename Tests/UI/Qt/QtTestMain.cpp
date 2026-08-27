/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibTest/TestSuite.h>

#include <QApplication>

int main(int argc, char** argv)
{
    if (argc < 1 || !argv[0] || '\0' == *argv[0]) {
        warnln("Test main does not have a valid test name!");
        return 1;
    }

    qputenv("QT_QPA_PLATFORM", "offscreen");
    int result = 0;
    {
        QApplication application(argc, argv);

        Vector<StringView> arguments;
        arguments.ensure_capacity(argc);
        for (auto i = 0; i < argc; ++i)
            arguments.append({ argv[i], strlen(argv[i]) });

        result = Test::TestSuite::the().main(argv[0], arguments);
        Test::TestSuite::release();
    }

    // QApplication is destroyed before returning so Qt releases resources
    // such as its font cache before LeakSanitizer runs.
    return result != 0;
}
