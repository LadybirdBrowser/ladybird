/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>

#if !defined(AK_OS_WINDOWS) && !defined(AK_OS_ANDROID)
#    include <editline/readline.h>
#endif

extern "C" {

using LineCompletionFunction = char** (*)(char const* line);

// JSLineEditor of the runtime's src/utilities/line_editor.rs.
struct JSLineEditor {
    char* (*readline)(char const* prompt);
    int (*add_history)(char const* line);
    int (*read_history)(char const* path);
    int (*write_history)(char const* path);
    void (*set_line_completion_function)(LineCompletionFunction complete_line);
};

int libjs_runtime_rust_js_main(int argc, char** argv, JSLineEditor const* line_editor);
}

#if !defined(AK_OS_WINDOWS) && !defined(AK_OS_ANDROID)
static LineCompletionFunction s_complete_line;

static char** complete_line_being_edited(char const*, int, int)
{
    if (!rl_line_buffer)
        return nullptr;
    rl_attempted_completion_over = 1;
    return s_complete_line(rl_line_buffer);
}

static constexpr JSLineEditor s_libedit_line_editor {
    .readline = readline,
    .add_history = add_history,
    .read_history = read_history,
    .write_history = write_history,
    .set_line_completion_function = [](LineCompletionFunction complete_line) {
        s_complete_line = complete_line;
        rl_attempted_completion_function = complete_line_being_edited;
    },
};
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
#if defined(AK_OS_WINDOWS) || defined(AK_OS_ANDROID)
    JSLineEditor const* line_editor = nullptr;
#else
    JSLineEditor const* line_editor = &s_libedit_line_editor;
#endif
    return libjs_runtime_rust_js_main(arguments.argc, arguments.argv, line_editor);
}
