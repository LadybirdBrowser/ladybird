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

TEST_CASE(breakpoints_resolve_to_source_map_entries)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("breakpoint.js"_utf16, 2));
    EXPECT_EQ(MUST(vm->debugger()->add_breakpoint("breakpoint.js"_utf16, 2)), breakpoint_id);

    auto script_or_error = JS::Script::parse("let first = 1;\nlet second = 2;\n"sv, realm, "breakpoint.js"sv);
    VERIFY(!script_or_error.is_error());
    EXPECT(vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    auto* executable = script_or_error.value()->cached_executable();
    VERIFY(executable);
    auto source_map_entry = executable->source_map.find_if([](auto const& entry) {
        return entry.line == 2;
    });
    VERIFY(!source_map_entry.is_end());
    EXPECT(executable->has_debugger_breakpoint_at(source_map_entry->bytecode_offset));

    EXPECT(vm->debugger()->remove_breakpoint(breakpoint_id));
    EXPECT(!executable->has_debugger_breakpoint_at(source_map_entry->bytecode_offset));
    EXPECT(!vm->debugger()->remove_breakpoint(breakpoint_id));
}

TEST_CASE(breakpoints_resolve_when_an_existing_executable_runs)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("let value = 1;\n"sv, realm, "existing.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("existing.js"_utf16, 1));
    EXPECT(!vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT(vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    auto* executable = script_or_error.value()->cached_executable();
    VERIFY(executable);
    vm->disable_debugging();
    EXPECT(!executable->has_debugger_breakpoint(breakpoint_id));
}

TEST_CASE(breakpoints_resolve_when_lazy_functions_are_compiled)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("lazy.js"_utf16, 2));

    auto declaration_or_error = JS::Script::parse("function lazy() {\n    let value = 1;\n    return value;\n}\n"sv, realm, "lazy.js"sv);
    VERIFY(!declaration_or_error.is_error());
    EXPECT(!vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    auto declaration_result = vm->run(*declaration_or_error.value());
    EXPECT(!declaration_result.is_error());
    EXPECT(!vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    auto call_or_error = JS::Script::parse("lazy();"_utf16, realm, "caller.js"sv);
    VERIFY(!call_or_error.is_error());
    auto call_result = vm->run(*call_or_error.value());
    EXPECT(!call_result.is_error());
    EXPECT(vm->debugger()->is_breakpoint_resolved(breakpoint_id));
}
TEST_CASE(breakpoints_slide_to_the_next_source_position)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("slide.js"_utf16, 2));

    auto script_or_error = JS::Script::parse("let first = 1;\n\nlet second = 2;\n"sv, realm, "slide.js"sv);
    VERIFY(!script_or_error.is_error());
    EXPECT(vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    auto* executable = script_or_error.value()->cached_executable();
    VERIFY(executable);
    auto source_map_entry = executable->source_map.find_if([](auto const& entry) {
        return entry.line == 3;
    });
    VERIFY(!source_map_entry.is_end());
    EXPECT(executable->has_debugger_breakpoint_at(source_map_entry->bytecode_offset));
}
