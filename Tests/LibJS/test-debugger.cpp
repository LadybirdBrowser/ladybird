/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Debugger.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
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

static Optional<JS::Value> find_environment_binding(JS::Debugger::FrameEnvironment const& environment, Utf16View name)
{
    auto binding = environment.bindings.find_if([&](auto const& binding) { return binding.name == name; });
    if (binding == environment.bindings.end())
        return {};
    return binding->value;
}

TEST_CASE(debugger_frame_environments_walk_from_the_locals_to_the_global_object)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(uR"(
let global = 1;
function outer() {
    let captured = 2;
    function inner(argument) {
        let local = captured + argument;
        debugger;
    }
    inner(3);
}
outer();
)"_utf16,
        realm, "environments.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto environments = vm->debugger()->environments_for_frame(*frame);
        auto index_of_environment_binding = [&](Utf16View name) {
            auto environment = environments.find_if([&](auto const& environment) { return find_environment_binding(environment, name).has_value(); });
            VERIFY(environment != environments.end());
            return environment.index();
        };

        auto const& locals = environments.first();
        EXPECT(locals.type == JS::Debugger::FrameEnvironment::Type::Function);
        EXPECT_EQ(locals.function_name, "inner"_utf16);
        EXPECT_EQ(find_environment_binding(locals, "argument"_utf16)->as_i32(), 3);
        EXPECT_EQ(find_environment_binding(locals, "local"_utf16)->as_i32(), 5);

        auto captured_index = index_of_environment_binding("captured"_utf16);
        auto global_index = index_of_environment_binding("global"_utf16);
        EXPECT(captured_index > 0);
        EXPECT(captured_index < global_index);
        EXPECT_EQ(find_environment_binding(environments[captured_index], "captured"_utf16)->as_i32(), 2);
        EXPECT(environments[global_index].type == JS::Debugger::FrameEnvironment::Type::Block);
        EXPECT_EQ(find_environment_binding(environments[global_index], "global"_utf16)->as_i32(), 1);

        auto const& global_object = environments.last();
        EXPECT(global_index + 1 == environments.size() - 1);
        EXPECT(global_object.type == JS::Debugger::FrameEnvironment::Type::Object);
        EXPECT_EQ(global_object.object.ptr(), &realm.global_environment().global_this_value());
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
}
