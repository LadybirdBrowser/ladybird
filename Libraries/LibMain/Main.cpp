/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>
#include <string.h>
#include <time.h>
#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

namespace Main {

static int s_return_code_for_errors = 1;

int return_code_for_errors()
{
    return s_return_code_for_errors;
}

void set_return_code_for_errors(int code)
{
    s_return_code_for_errors = code;
}

}

int main(int argc, char** argv)
{
    tzset();

#if defined(AK_OS_WINDOWS)
    windows_init();
#else
    // Raise the open file limit well above the platform default. Each decoded image is backed by its own shared-memory
    // file descriptor — so a document with thousands of images (or many open tabs) otherwise exhausts the descriptor
    // table, and aborts when the next descriptor is sent over IPC.
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 65536); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());
#endif

    Vector<StringView> arguments;
    arguments.ensure_capacity(argc);
    for (int i = 0; i < argc; ++i)
        arguments.unchecked_append({ argv[i], strlen(argv[i]) });

    auto result = ladybird_main({
        .argc = argc,
        .argv = argv,
        .strings = arguments.span(),
    });

#if defined(AK_OS_WINDOWS)
    windows_shutdown();
#endif

    if (result.is_error()) {
        auto error = result.release_error();
        warnln("\033[31;1mRuntime error\033[0m: {}", error);
        return Main::return_code_for_errors();
    }
    return result.value();
}
