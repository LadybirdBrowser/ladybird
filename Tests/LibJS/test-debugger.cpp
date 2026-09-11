/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Op.h>
#include <LibJS/Debugger.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/Script.h>
#include <LibJS/SourceTextModule.h>
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
        EXPECT_EQ(pause_info.bytecode_offset, sizeof(JS::Bytecode::Op::Enter));
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

TEST_CASE(debugger_pause_reports_the_active_stack)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(R"(
function outer() {
    inner();
}
function inner() {
    debugger;
}
outer();
)"sv,
        realm, "stack.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        EXPECT(pause_info.stack_trace.size() >= 3);
        EXPECT_EQ(pause_info.stack_trace[0].execution_context->executable.ptr(), pause_info.executable.ptr());
        VERIFY(pause_info.stack_trace[0].source_range.has_value());
        EXPECT_EQ(pause_info.stack_trace[0].source_range->start.line, 6u);

        size_t script_frame_count = 0;
        for (auto const& frame : pause_info.stack_trace) {
            if (frame.execution_context->executable)
                ++script_frame_count;
        }
        EXPECT_EQ(script_frame_count, 3u);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
}

TEST_CASE(debugger_can_evaluate_in_a_paused_frame)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(R"(
function answer()
{
    let value = 41;
    debugger;
    return value;
}
answer();
)"sv,
        realm, "evaluate.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto result = vm->debugger()->evaluate_in_frame(*frame, "value + 1"sv);
        VERIFY(!result.is_error());
        EXPECT_EQ(result.release_value().as_i32(), 42);

        result = vm->debugger()->evaluate_in_frame(*frame, "value = 50"sv);
        VERIFY(!result.is_error());
        EXPECT_EQ(result.release_value().as_i32(), 50);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    VERIFY(!result.is_error());
    EXPECT_EQ(result.release_value().as_i32(), 50);
}

TEST_CASE(debugger_frame_evaluation_exposes_and_updates_parameters)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("function update(value) { debugger; return value; } update(41);"sv, realm, "parameters.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto result = vm->debugger()->evaluate_in_frame(*frame, "value += 1"sv);
        VERIFY(!result.is_error());
        EXPECT_EQ(result.release_value().as_i32(), 42);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    VERIFY(!result.is_error());
    EXPECT_EQ(result.release_value().as_i32(), 42);
}

TEST_CASE(debugger_frame_evaluation_exposes_non_simple_parameters)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("function update(value = 41) { debugger; return value; } update();"sv, realm, "non-simple-parameters.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto result = vm->debugger()->evaluate_in_frame(*frame, "value += 1"sv);
        VERIFY(!result.is_error());
        EXPECT_EQ(result.release_value().as_i32(), 42);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    VERIFY(!result.is_error());
    EXPECT_EQ(result.release_value().as_i32(), 42);
}

TEST_CASE(debugger_frame_bindings_include_active_arguments_and_locals)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(R"(
function inspect(argument) {
    const immutable = 2;
    {
        let inactive = 3;
    }
    let mutable = 4;
    debugger;
}
inspect(1);
)"sv,
        realm, "bindings.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto bindings = vm->debugger()->bindings_for_frame(*frame);
        auto find_binding = [&](Utf16View name) {
            return bindings.find_if([&](auto const& binding) { return binding.name == name; });
        };

        auto argument = find_binding("argument"_utf16);
        VERIFY(argument != bindings.end());
        EXPECT_EQ(argument->value.as_i32(), 1);
        EXPECT(argument->is_mutable);

        auto immutable = find_binding("immutable"_utf16);
        VERIFY(immutable != bindings.end());
        EXPECT_EQ(immutable->value.as_i32(), 2);
        EXPECT(!immutable->is_mutable);

        auto mutable_binding = find_binding("mutable"_utf16);
        VERIFY(mutable_binding != bindings.end());
        EXPECT_EQ(mutable_binding->value.as_i32(), 4);
        EXPECT(mutable_binding->is_mutable);

        EXPECT(find_binding("inactive"_utf16) == bindings.end());
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
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

    auto script_or_error = JS::Script::parse(R"(
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
)"sv,
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

TEST_CASE(debugger_frame_environments_include_module_bindings)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto module_or_error = JS::SourceTextModule::parse(R"(
const value = 1;
function inner() {
    debugger;
}
inner();
)"sv,
        realm, "environments.mjs"sv);
    VERIFY(!module_or_error.is_error());

    size_t pause_count = 0;
    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto environments = vm->debugger()->environments_for_frame(*frame);
        auto module_environment = environments.find_if([](auto const& environment) { return find_environment_binding(environment, "value"_utf16).has_value(); });
        VERIFY(module_environment != environments.end());
        EXPECT_EQ(find_environment_binding(*module_environment, "value"_utf16)->as_i32(), 1);
        vm->debugger()->continue_execution();
    });

    auto& module = *module_or_error.value();
    (void)module.load_requested_modules(nullptr);
    EXPECT(!module.link(*vm).is_error());
    EXPECT(!module.evaluate(*vm).is_error());
    EXPECT_EQ(pause_count, 1u);
}

static void check_this_value_of_paused_frame(StringView source, size_t expected_pause_count, Function<void(JS::Value this_value, JS::Realm&)> check)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(source, realm, "this.js"sv);
    VERIFY(!script_or_error.is_error());

    size_t pause_count = 0;
    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);
        check(vm->debugger()->this_value_for_frame(*frame), realm);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, expected_pause_count);
}

TEST_CASE(debugger_frame_this_value_is_recovered_for_functions_that_never_use_it)
{
    check_this_value_of_paused_frame(R"(
function sloppy() {
    debugger;
}
sloppy();
)"sv,
        1, [](JS::Value this_value, JS::Realm& realm) {
            EXPECT(this_value.is_object() && &this_value.as_object() == &realm.global_environment().global_this_value());
        });

    check_this_value_of_paused_frame(R"(
function strict() {
    'use strict';
    debugger;
}
strict();
)"sv,
        1, [](JS::Value this_value, JS::Realm&) {
            EXPECT(this_value.is_undefined());
        });

    check_this_value_of_paused_frame(R"(
function outer() {
    const arrow = () => {
        debugger;
    };
    arrow();
}
outer();
)"sv,
        1, [](JS::Value this_value, JS::Realm& realm) {
            EXPECT(this_value.is_object() && &this_value.as_object() == &realm.global_environment().global_this_value());
        });

    check_this_value_of_paused_frame(R"(
var receiver = {
    method() {
        debugger;
        return this;
    },
};
receiver.method();
)"sv,
        1, [](JS::Value this_value, JS::Realm& realm) {
            auto receiver = MUST(realm.global_object().get("receiver"_utf16));
            EXPECT(this_value.is_object() && &this_value.as_object() == &receiver.as_object());
        });
}

static void expect_this_value_is_the_global_receiver(JS::Value this_value, JS::Realm& realm)
{
    auto receiver = MUST(realm.global_object().get("receiver"_utf16));
    EXPECT(this_value.is_object() && &this_value.as_object() == &receiver.as_object());
}

TEST_CASE(debugger_frame_this_value_keeps_the_receiver_of_functions_that_never_use_it)
{
    // Every function is called twice: the first call compiles it on the slow path, the second one is inlined.
    check_this_value_of_paused_frame(R"(
var receiver = {
    method() {
        debugger;
    },
};
receiver.method();
receiver.method();
)"sv,
        2, expect_this_value_is_the_global_receiver);

    check_this_value_of_paused_frame(R"(
var receiver = {
    method() {
        let captured = 1;
        const read = () => captured;
        debugger;
        read();
    },
};
receiver.method();
receiver.method();
)"sv,
        2, expect_this_value_is_the_global_receiver);

    check_this_value_of_paused_frame(R"(
var receiver = {
    method() {
        const arrow = () => {
            debugger;
        };
        arrow();
    },
};
receiver.method();
receiver.method();
)"sv,
        2, expect_this_value_is_the_global_receiver);

    check_this_value_of_paused_frame(R"(
var receiver = {};
[1, 2].forEach(function () {
    debugger;
}, receiver);
)"sv,
        2, expect_this_value_is_the_global_receiver);

    // The arrow is created by the outer invocation but runs inside a recursive one with a different receiver.
    check_this_value_of_paused_frame(R"(
var receiver = {};
var other = {};
function method(arrow) {
    let level = 0;
    if (arrow) {
        arrow();
        return;
    }
    method.call(other, () => {
        debugger;
        level;
    });
}
method.call(receiver);
)"sv,
        1, expect_this_value_is_the_global_receiver);

    check_this_value_of_paused_frame(R"(
function Constructor() {
    debugger;
}
new Constructor();
new Constructor();
)"sv,
        2, [](JS::Value this_value, JS::Realm& realm) {
            auto constructor = MUST(realm.global_object().get("Constructor"_utf16));
            EXPECT(this_value.is_object());
            if (this_value.is_object())
                EXPECT_EQ(MUST(this_value.as_object().get("constructor"_utf16)), constructor);
        });

    check_this_value_of_paused_frame(R"(
String.prototype.describe = function () {
    debugger;
};
'abc'.describe();
'abc'.describe();
)"sv,
        2, [](JS::Value this_value, JS::Realm&) {
            EXPECT(this_value.is_object());
            if (this_value.is_object())
                EXPECT_EQ(MUST(this_value.as_object().get("length"_utf16)).as_i32(), 3);
        });

    check_this_value_of_paused_frame(R"(
String.prototype.describe = function () {
    'use strict';
    debugger;
};
'abc'.describe();
'abc'.describe();
)"sv,
        2, [](JS::Value this_value, JS::Realm&) {
            EXPECT(this_value.is_string());
        });
}

TEST_CASE(debugger_frame_evaluation_preserves_const_bindings)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("function read() { const value = 41; debugger; return value; } read();"sv, realm, "const.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);
        EXPECT(vm->debugger()->evaluate_in_frame(*frame, "value = 42"sv).is_error());
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    VERIFY(!result.is_error());
    EXPECT_EQ(result.release_value().as_i32(), 41);
}

TEST_CASE(debugger_frame_evaluation_does_not_overwrite_shadowed_locals)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(R"(
function readOuterValue()
{
    let value = 1;
    {
        let value = 2;
        debugger;
    }
    return value;
}
readOuterValue();
)"sv,
        realm, "shadowed-locals.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);

        auto result = vm->debugger()->evaluate_in_frame(*frame, "value"sv);
        VERIFY(!result.is_error());
        EXPECT_EQ(result.release_value().as_i32(), 2);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    VERIFY(!result.is_error());
    EXPECT_EQ(result.release_value().as_i32(), 1);
}

TEST_CASE(debugger_frame_evaluation_uses_the_active_shadowed_binding)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(R"(
function readValue()
{
    let value = 1;
    debugger;
    {
        let value = 2;
        debugger;
    }
    debugger;
}
readValue();
)"sv,
        realm, "shadowed-live-ranges.js"sv);
    VERIFY(!script_or_error.is_error());

    Vector<i32> values;
    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        auto* frame = pause_info.stack_trace.first().execution_context;
        VERIFY(frame);
        auto result = vm->debugger()->evaluate_in_frame(*frame, "value"sv);
        VERIFY(!result.is_error());
        values.append(result.release_value().as_i32());
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(values, (Vector<i32> { 1, 2, 1 }));
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

TEST_CASE(source_specific_breakpoints_do_not_match_other_sources_with_the_same_filename)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto first_script = MUST(JS::Script::parse("let first = 1;\n"sv, realm, "shared.js"sv));
    auto* first_executable = first_script->cached_executable();
    VERIFY(first_executable);

    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint(first_executable->source_code, 1));
    EXPECT(first_executable->has_debugger_breakpoint(breakpoint_id));

    auto second_script = MUST(JS::Script::parse("let second = 2;\n"sv, realm, "shared.js"sv));
    auto* second_executable = second_script->cached_executable();
    VERIFY(second_executable);
    EXPECT(!second_executable->has_debugger_breakpoint(breakpoint_id));
}

TEST_CASE(debugger_source_reports_breakpoint_positions_in_lazy_functions)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(R"(
        /top-level/;
        button.onclick = function () {
            /lazy/;
            alert("Hi!");
            alert("Hello!");
            alert("Hey!");
        };
    )"sv,
        realm, "lazy-positions.js"sv);
    VERIFY(!script_or_error.is_error());

    auto* executable = script_or_error.value()->cached_executable();
    VERIFY(executable);
    auto positions = JS::RustIntegration::breakpoint_positions_for_source(*executable->source_code, JS::RustIntegration::ProgramType::Script, 1);
    for (u32 line = 5; line <= 7; ++line)
        EXPECT(positions.find_if([line](auto const& position) { return position.line == line; }) != positions.end());
}

TEST_CASE(module_breakpoint_positions_apply_the_source_line_offset)
{
    auto source_code = JS::SourceCode::create("inline-module.js"_utf16, "export const value = 1;"_utf16);
    auto positions = JS::RustIntegration::breakpoint_positions_for_source(*source_code, JS::RustIntegration::ProgramType::Module, 20);
    EXPECT(positions.find_if([](auto const& position) { return position.line == 20; }) != positions.end());
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

TEST_CASE(breakpoints_slide_to_the_closest_position_across_nested_executables)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto declaration_or_error = JS::Script::parse(
        "function target() {\n"
        "    // Breakpoint requested here.\n"
        "    let inside = 1;\n"
        "}\n"
        "let outside = 2;\n"sv,
        realm, "nested-slide.js"sv);
    VERIFY(!declaration_or_error.is_error());

    vm->enable_debugging();
    auto declaration_result = vm->run(*declaration_or_error.value());
    EXPECT(!declaration_result.is_error());

    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("nested-slide.js"_utf16, 2));
    auto* top_level_executable = declaration_or_error.value()->cached_executable();
    VERIFY(top_level_executable);
    EXPECT(top_level_executable->has_debugger_breakpoint(breakpoint_id));

    Vector<u32> paused_lines;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        VERIFY(pause_info.source_range.has_value());
        paused_lines.append(pause_info.source_range->start.line);
        vm->debugger()->continue_execution();
    });

    auto call_or_error = JS::Script::parse("target();"_utf16, realm, "caller.js"sv);
    VERIFY(!call_or_error.is_error());
    auto call_result = vm->run(*call_or_error.value());
    EXPECT(!call_result.is_error());

    EXPECT_EQ(paused_lines, (Vector<u32> { 3 }));
    EXPECT(!top_level_executable->has_debugger_breakpoint(breakpoint_id));
}

TEST_CASE(column_breakpoints_do_not_slide_onto_the_enclosing_executable)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto script_or_error = JS::Script::parse(
        "function target() {\n"
        "    let inside = 1;\n"
        "    return inside;\n"
        "}\n"
        "target();\n"sv,
        realm, "column.js"sv);
    VERIFY(!script_or_error.is_error());
    auto* top_level_executable = script_or_error.value()->cached_executable();
    VERIFY(top_level_executable);

    auto positions = JS::RustIntegration::breakpoint_positions_for_source(*top_level_executable->source_code, JS::RustIntegration::ProgramType::Script, 1);
    auto position = positions.find_if([](auto const& position) { return position.line == 3; });
    VERIFY(!position.is_end());

    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("column.js"_utf16, 3, position->column));
    EXPECT(!top_level_executable->has_debugger_breakpoint(breakpoint_id));

    Vector<u32> paused_lines;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        VERIFY(pause_info.source_range.has_value());
        paused_lines.append(pause_info.source_range->start.line);
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(paused_lines, (Vector<u32> { 3 }));
}

TEST_CASE(breakpoints_resolve_when_precompiled_functions_are_called_inline)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto declaration_or_error = JS::Script::parse("function target() {\n    return 42;\n}\n"sv, realm, "inline.js"sv);
    VERIFY(!declaration_or_error.is_error());
    auto declaration_result = vm->run(*declaration_or_error.value());
    EXPECT(!declaration_result.is_error());

    auto call_or_error = JS::Script::parse("target();"_utf16, realm, "caller.js"sv);
    VERIFY(!call_or_error.is_error());
    auto call_result = vm->run(*call_or_error.value());
    EXPECT(!call_result.is_error());

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("inline.js"_utf16, 2));
    EXPECT(!vm->debugger()->is_breakpoint_resolved(breakpoint_id));

    call_result = vm->run(*call_or_error.value());
    EXPECT(!call_result.is_error());
    EXPECT(vm->debugger()->is_breakpoint_resolved(breakpoint_id));
}

TEST_CASE(manual_breakpoints_pause_execution)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("manual.js"_utf16, 2));

    size_t pause_count = 0;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        EXPECT_EQ(pause_info.reason, JS::Debugger::PauseReason::Breakpoint);
        VERIFY(pause_info.source_range.has_value());
        EXPECT_EQ(pause_info.source_range->start.line, 2u);
        EXPECT_EQ(pause_info.breakpoint_ids.size(), 1u);
        EXPECT_EQ(pause_info.breakpoint_ids.first(), breakpoint_id);
        vm->debugger()->continue_execution();
    });

    auto script_or_error = JS::Script::parse("var first = 1;\nvar second = 2;\n"sv, realm, "manual.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 1u);

    EXPECT(vm->debugger()->remove_breakpoint(breakpoint_id));
    result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 1u);
}

TEST_CASE(pause_on_next_bytecode_execution_is_one_shot)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    vm->debugger()->request_pause_on_next_bytecode_execution();

    size_t pause_count = 0;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        EXPECT_EQ(pause_info.reason, JS::Debugger::PauseReason::Entry);
        VERIFY(pause_info.source_range.has_value());
        vm->debugger()->continue_execution();
    });

    auto script_or_error = JS::Script::parse("42;"sv, realm, "entry.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 1u);

    result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 1u);
}

TEST_CASE(pause_on_empty_script_waits_for_frame_initialization)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse(""sv, realm, "empty.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->request_pause_on_next_bytecode_execution();

    bool did_pause = false;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        did_pause = true;
        EXPECT(vm->running_execution_context().frame_initialized);
        EXPECT_EQ(pause_info.bytecode_offset, sizeof(JS::Bytecode::Op::Enter));
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT(did_pause);
}

TEST_CASE(pause_on_next_bytecode_execution_waits_for_a_callback)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto script_or_error = JS::Script::parse("42;"sv, realm, "entry.js"sv);
    VERIFY(!script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->request_pause_on_next_bytecode_execution();
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());

    size_t pause_count = 0;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const&) {
        ++pause_count;
        vm->debugger()->continue_execution();
    });

    result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 1u);
}

TEST_CASE(nested_execution_does_not_consume_a_pause_request)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto outer_script_or_error = JS::Script::parse("let first = 1;\nlet second = 2;\n"sv, realm, "outer.js"sv);
    VERIFY(!outer_script_or_error.is_error());
    auto nested_script_or_error = JS::Script::parse("1 + 1;"sv, realm, "watch.js"sv);
    VERIFY(!nested_script_or_error.is_error());

    vm->enable_debugging();
    vm->debugger()->request_pause_on_next_bytecode_execution();

    size_t pause_count = 0;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const&) {
        ++pause_count;
        if (pause_count == 1) {
            vm->debugger()->request_pause_on_next_bytecode_execution();
            auto nested_result = vm->run(*nested_script_or_error.value());
            EXPECT(!nested_result.is_error());
            EXPECT_EQ(pause_count, 1u);
        }
        vm->debugger()->continue_execution();
    });

    auto result = vm->run(*outer_script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 2u);
}

TEST_CASE(manual_breakpoints_replace_debugger_statement_pauses)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    MUST(vm->debugger()->add_breakpoint("combined.js"_utf16, 1));

    Vector<JS::Debugger::PauseReason> pause_reasons;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        pause_reasons.append(pause_info.reason);
        vm->debugger()->continue_execution();
    });

    auto script_or_error = JS::Script::parse("debugger;"sv, realm, "combined.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_reasons.size(), 1u);
    EXPECT_EQ(pause_reasons[0], JS::Debugger::PauseReason::Breakpoint);
}

TEST_CASE(step_into_pauses_in_a_called_function)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("step-into.js"_utf16, 4));

    Vector<JS::Debugger::PauseInfo> pauses;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        pauses.append(pause_info);
        if (pauses.size() == 1)
            EXPECT(vm->debugger()->remove_breakpoint(breakpoint_id));
        vm->debugger()->continue_execution(pauses.size() == 1 ? JS::Debugger::ResumeMode::StepInto : JS::Debugger::ResumeMode::Continue);
    });

    auto script_or_error = JS::Script::parse(
        "function callee() {\n"
        "    let inside = 1;\n"
        "}\n"
        "callee();\n"
        "let after = 2;\n"sv,
        realm, "step-into.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());

    EXPECT_EQ(pauses.size(), 2u);
    EXPECT_EQ(pauses[0].reason, JS::Debugger::PauseReason::Breakpoint);
    EXPECT_EQ(pauses[1].reason, JS::Debugger::PauseReason::Step);
    EXPECT_EQ(pauses[1].source_range->start.line, 2u);
}

TEST_CASE(step_over_does_not_pause_in_called_functions)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("step-over.js"_utf16, 4));

    Vector<JS::Debugger::PauseInfo> pauses;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        pauses.append(pause_info);
        if (pauses.size() == 1)
            EXPECT(vm->debugger()->remove_breakpoint(breakpoint_id));
        vm->debugger()->continue_execution(pauses.size() == 1 ? JS::Debugger::ResumeMode::StepOver : JS::Debugger::ResumeMode::Continue);
    });

    auto script_or_error = JS::Script::parse(
        "function callee() {\n"
        "    let inside = 1;\n"
        "}\n"
        "callee();\n"
        "let after = 2;\n"sv,
        realm, "step-over.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());

    EXPECT_EQ(pauses.size(), 2u);
    EXPECT_EQ(pauses[0].reason, JS::Debugger::PauseReason::Breakpoint);
    EXPECT_EQ(pauses[1].reason, JS::Debugger::PauseReason::Step);
    EXPECT_EQ(pauses[1].source_range->start.line, 5u);
}

TEST_CASE(step_out_pauses_after_the_current_function_returns)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();

    Vector<JS::Debugger::PauseInfo> pauses;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        pauses.append(pause_info);
        vm->debugger()->continue_execution(pauses.size() == 1 ? JS::Debugger::ResumeMode::StepOut : JS::Debugger::ResumeMode::Continue);
    });

    auto script_or_error = JS::Script::parse(
        "function callee() {\n"
        "    debugger;\n"
        "}\n"
        "callee();\n"
        "let after = 2;\n"sv,
        realm, "step-out.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());

    EXPECT_EQ(pauses.size(), 2u);
    VERIFY(pauses.size() == 2);
    EXPECT_EQ(pauses[0].reason, JS::Debugger::PauseReason::DebuggerStatement);
    EXPECT_EQ(pauses[1].reason, JS::Debugger::PauseReason::Step);
    EXPECT_EQ(pauses[1].source_range->start.line, 5u);
}

TEST_CASE(step_out_distinguishes_reused_inline_frames)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();

    realm.global_object().define_native_function(realm, "runBoth"_utf16_fly_string, Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> { [](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        TRY(JS::call(vm, vm.argument(0), JS::js_undefined()));
        return JS::call(vm, vm.argument(1), JS::js_undefined());
    } },
        2, JS::default_attributes);

    Vector<JS::Debugger::PauseInfo> pauses;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        pauses.append(pause_info);
        vm->debugger()->continue_execution(pauses.size() == 1 ? JS::Debugger::ResumeMode::StepOut : JS::Debugger::ResumeMode::Continue);
    });

    auto script_or_error = JS::Script::parse(
        "function first() { debugger; }\n"
        "function second() { let inside = 1; }\n"
        "runBoth(first, second);\n"
        "let after = 2;\n"sv,
        realm, "step-out-reused-frame.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());

    EXPECT_EQ(pauses.size(), 2u);
    VERIFY(pauses.size() == 2);
    EXPECT_EQ(pauses[0].reason, JS::Debugger::PauseReason::DebuggerStatement);
    EXPECT_EQ(pauses[1].reason, JS::Debugger::PauseReason::Step);
    EXPECT_EQ(pauses[1].source_range->start.line, 2u);
}

TEST_CASE(ignored_step_pauses_preserve_the_active_step)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    auto breakpoint_id = MUST(vm->debugger()->add_breakpoint("ignored-step.js"_utf16, 4));

    Vector<JS::Debugger::PauseInfo> pauses;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        pauses.append(pause_info);
        if (pauses.size() == 1) {
            EXPECT(vm->debugger()->remove_breakpoint(breakpoint_id));
            vm->debugger()->continue_execution(JS::Debugger::ResumeMode::StepInto);
        } else if (pauses.size() == 2) {
            vm->debugger()->continue_execution_preserving_step_state();
        } else {
            vm->debugger()->continue_execution();
        }
    });

    auto script_or_error = JS::Script::parse(
        "function callee() {\n"
        "    let inside = 1;\n"
        "}\n"
        "callee();\n"
        "let after = 2;\n"sv,
        realm, "ignored-step.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());

    EXPECT_EQ(pauses.size(), 3u);
    VERIFY(pauses.size() == 3);
    EXPECT_EQ(pauses[1].reason, JS::Debugger::PauseReason::Step);
    EXPECT_EQ(pauses[1].source_range->start.line, 2u);
    EXPECT_EQ(pauses[2].reason, JS::Debugger::PauseReason::Step);
    EXPECT_EQ(pauses[2].source_range->start.line, 5u);
}

TEST_CASE(step_state_does_not_survive_its_bytecode_execution)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    size_t pause_count = 0;
    vm->enable_debugging();
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const&) {
        ++pause_count;
        vm->debugger()->continue_execution(JS::Debugger::ResumeMode::StepInto);
    });

    auto first_script = JS::Script::parse("debugger;"sv, realm, "first.js"sv).release_value();
    EXPECT(!vm->run(*first_script).is_error());
    EXPECT_EQ(pause_count, 1u);

    auto second_script = JS::Script::parse("let later = 1;"sv, realm, "second.js"sv).release_value();
    EXPECT(!vm->run(*second_script).is_error());
    EXPECT_EQ(pause_count, 1u);
}

TEST_CASE(pause_on_all_exceptions_reports_caught_exceptions)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    vm->debugger()->set_pause_on_exceptions(JS::Debugger::PauseOnExceptions::All);

    size_t pause_count = 0;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        EXPECT_EQ(pause_info.reason, JS::Debugger::PauseReason::Exception);
        EXPECT(pause_info.exception.has_value());
        EXPECT_EQ(pause_info.exception->as_i32(), 42);
        EXPECT(pause_info.exception_will_be_caught);
        vm->debugger()->continue_execution();
    });

    auto script_or_error = JS::Script::parse("try { throw 42; } catch {}"sv, realm, "caught.js"sv);
    VERIFY(!script_or_error.is_error());
    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 1u);
}

TEST_CASE(pause_on_uncaught_exceptions_ignores_caught_exceptions)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    vm->enable_debugging();
    vm->debugger()->set_pause_on_exceptions(JS::Debugger::PauseOnExceptions::Uncaught);

    size_t pause_count = 0;
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        EXPECT_EQ(pause_info.reason, JS::Debugger::PauseReason::Exception);
        EXPECT(pause_info.exception.has_value());
        EXPECT_EQ(pause_info.exception->as_i32(), 43);
        EXPECT(!pause_info.exception_will_be_caught);
        vm->debugger()->continue_execution();
    });

    auto caught_script_or_error = JS::Script::parse("try { throw 42; } catch {}"sv, realm, "caught.js"sv);
    VERIFY(!caught_script_or_error.is_error());
    auto result = vm->run(*caught_script_or_error.value());
    EXPECT(!result.is_error());
    EXPECT_EQ(pause_count, 0u);

    auto uncaught_script_or_error = JS::Script::parse("throw 43;"sv, realm, "uncaught.js"sv);
    VERIFY(!uncaught_script_or_error.is_error());
    result = vm->run(*uncaught_script_or_error.value());
    EXPECT(result.is_error());
    EXPECT_EQ(pause_count, 1u);
}

TEST_CASE(pause_on_uncaught_exceptions_reports_the_original_throw_through_finally)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    Vector<u32> paused_lines;
    vm->enable_debugging();
    vm->debugger()->set_pause_on_exceptions(JS::Debugger::PauseOnExceptions::Uncaught);
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        EXPECT(!pause_info.exception_will_be_caught);
        VERIFY(pause_info.source_range.has_value());
        paused_lines.append(pause_info.source_range->start.line);
        vm->debugger()->continue_execution();
    });

    auto script = JS::Script::parse("try {\n    throw 42;\n} finally {\n    let cleanup = 1;\n}"sv, realm, "finally.js"sv).release_value();
    EXPECT(vm->run(*script).is_error());
    EXPECT_EQ(paused_lines, (Vector<u32> { 2 }));
}

TEST_CASE(pause_on_uncaught_exceptions_preserves_native_callback_frames)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    size_t pause_count = 0;
    vm->enable_debugging();
    vm->debugger()->set_pause_on_exceptions(JS::Debugger::PauseOnExceptions::Uncaught);
    vm->debugger()->set_pause_callback([&](JS::Debugger::PauseInfo const& pause_info) {
        ++pause_count;
        EXPECT(!pause_info.exception_will_be_caught);
        VERIFY(pause_info.source_range.has_value());
        EXPECT_EQ(pause_info.source_range->start.line, 2u);
        VERIFY(!pause_info.stack_trace.is_empty());
        EXPECT(pause_info.stack_trace.first().execution_context->function);
        vm->debugger()->continue_execution();
    });

    auto script = JS::Script::parse("[1].map(() => {\n    throw 42;\n});"sv, realm, "native-callback.js"sv).release_value();
    EXPECT(vm->run(*script).is_error());
    EXPECT_EQ(pause_count, 1u);
}
