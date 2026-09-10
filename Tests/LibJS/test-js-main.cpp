/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto executable = TRY(Core::System::current_executable_path());
    Vector<ByteString> runner_arguments;
    runner_arguments.append(LexicalPath::join(LexicalPath(executable).dirname(), "test-js.py"sv).string());
    for (auto argument : arguments.strings.slice(1))
        runner_arguments.append(argument);
    auto process = TRY(Core::Process::spawn(TEST_JS_PYTHON ""sv, runner_arguments.span()));
    return process.wait_for_termination();
}
