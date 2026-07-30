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

TEST_CASE(debugger_statement_pauses_execution)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("debugger; 42;"sv, realm, "debugger.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();

    bool did_pause = false;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        did_pause = true;
        EXPECT_EQ(pause_info.reason, JS::Debugger::PauseReason::DebuggerStatement);
        EXPECT_EQ(pause_info.bytecode_offset, 0u);
        VERIFY(pause_info.source_range.has_value());
        EXPECT_EQ(pause_info.source_range->filename(), "debugger.js"_utf16);
        EXPECT_EQ(pause_info.source_range->start.line, 1u);
        EXPECT_EQ(pause_info.source_range->start.column, 1u);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT(did_pause);
}

TEST_CASE(debugger_statement_continues_without_pause_callback)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("debugger; 42;"sv, realm);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
}
