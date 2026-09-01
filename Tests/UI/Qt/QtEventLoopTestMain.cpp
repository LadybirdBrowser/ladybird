/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibTest/TestSuite.h>
#include <UI/Qt/EventLoopImplementationQt.h>

#include <QCoreApplication>

int main(int argc, char** argv)
{
    if (argc < 1 || !argv[0] || '\0' == *argv[0]) {
        warnln("Test main does not have a valid test name!");
        return 1;
    }

    QCoreApplication application(argc, argv);
    Core::EventLoopManager::install(*new Ladybird::EventLoopManagerQt);

    Vector<StringView> arguments;
    arguments.ensure_capacity(argc);
    for (auto i = 0; i < argc; ++i)
        arguments.append({ argv[i], strlen(argv[i]) });

    auto result = Test::TestSuite::the().main(argv[0], arguments);
    Test::TestSuite::release();
    return result != 0;
}
