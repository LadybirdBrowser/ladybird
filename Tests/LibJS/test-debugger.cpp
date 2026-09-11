/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Debugger.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>

TEST_CASE(column_breakpoints_do_not_slide_onto_the_enclosing_executable)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto script = MUST(JS::Script::parse(
        u"function target() {\n"
        u"    let inside = 1;\n"
        u"    return inside;\n"
        u"}\n"
        u"target();\n"_utf16,
        realm, "column.js"sv));

    MUST(vm->debugger()->add_breakpoint("column.js"_utf16, 3, 5));
    Vector<u32> paused_lines;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        VERIFY(pause_info.source_range.has_value());
        paused_lines.append(pause_info.source_range->start.line);
        vm->debugger()->continue_execution();
    });

    EXPECT(!vm->run(*script).is_error());
    EXPECT_EQ(paused_lines, (Vector<u32> { 3 }));
}
