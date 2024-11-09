/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibMain/Main.h>
#include <string.h>
#include <time.h>

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

    Vector<StringView> arguments;
    arguments.ensure_capacity(argc);
    for (int i = 0; i < argc; ++i)
        arguments.unchecked_append({ argv[i], strlen(argv[i]) });

    auto result = serenity_main({
        .argc = argc,
        .argv = argv,
        .strings = arguments.span(),
    });
    if (result.is_error()) {
        auto error = result.release_error();
        warnln("\033[31;1mRuntime error\033[0m: {}", error);
        return Main::return_code_for_errors();
    }
    return result.value();
}
