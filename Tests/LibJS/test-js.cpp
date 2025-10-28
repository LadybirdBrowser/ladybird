/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibTest/JavaScriptTestRunner.h>
#include <LibUnicode/TimeZone.h>

TEST_ROOT("Libraries/LibJS/Tests");

TESTJS_PROGRAM_FLAG(test262_parser_tests, "Run test262 parser tests", "test262-parser-tests", 0);

TESTJS_GLOBAL_FUNCTION(can_parse_source, canParseSource)
{
    auto source = TRY(vm.argument(0).to_string(vm));
    auto parser = JS::Parser(JS::Lexer(source));
    (void)parser.parse_program();
    return JS::Value(!parser.has_errors());
}

// Based on $262.evalScript
TESTJS_GLOBAL_FUNCTION(evaluate_source, evaluateSource)
{
    auto& realm = *vm.current_realm();

    auto source = TRY(vm.argument(0).to_string(vm));

    auto script = JS::Script::parse(source, realm);
    if (script.is_error())
        return vm.throw_completion<JS::SyntaxError>(script.error().first().to_string());

    return vm.bytecode_interpreter().run(script.value());
}

TESTJS_GLOBAL_FUNCTION(run_queued_promise_jobs, runQueuedPromiseJobs)
{
    vm.run_queued_promise_jobs();
    return JS::js_undefined();
}

TESTJS_GLOBAL_FUNCTION(get_weak_set_size, getWeakSetSize)
{
    auto object = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::WeakSet>(*object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "WeakSet");
    auto& weak_set = static_cast<JS::WeakSet&>(*object);
    return JS::Value(weak_set.values().size());
}

TESTJS_GLOBAL_FUNCTION(get_weak_map_size, getWeakMapSize)
{
    auto object = TRY(vm.argument(0).to_object(vm));
    if (!is<JS::WeakMap>(*object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "WeakMap");
    auto& weak_map = static_cast<JS::WeakMap&>(*object);
    return JS::Value(weak_map.values().size());
}

TESTJS_GLOBAL_FUNCTION(mark_as_garbage, markAsGarbage)
{
    auto argument = vm.argument(0);
    if (!argument.is_string())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAString, argument.to_string_without_side_effects());

    auto& variable_name = argument.as_string();

    // In native functions we don't have a lexical environment so get the outer via the execution stack.
    auto outer_environment = vm.execution_context_stack().last_matching([&](auto& execution_context) {
        return execution_context->lexical_environment != nullptr;
    });
    if (!outer_environment.has_value())
        return vm.throw_completion<JS::ReferenceError>(JS::ErrorType::UnknownIdentifier, variable_name.utf8_string_view());

    auto reference = TRY(vm.resolve_binding(variable_name.utf16_string(), JS::Strict::No, outer_environment.value()->lexical_environment));

    auto value = TRY(reference.get_value(vm));

    if (!can_be_held_weakly(value))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::CannotBeHeldWeakly, ByteString::formatted("Variable with name {}", variable_name.utf8_string_view()));

    vm.heap().uproot_cell(&value.as_cell());
    TRY(reference.delete_(vm));

    return JS::js_undefined();
}

TESTJS_GLOBAL_FUNCTION(detach_array_buffer, detachArrayBuffer)
{
    auto array_buffer = vm.argument(0);
    if (!array_buffer.is_object() || !is<JS::ArrayBuffer>(array_buffer.as_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "ArrayBuffer");

    auto& array_buffer_object = static_cast<JS::ArrayBuffer&>(array_buffer.as_object());
    TRY(JS::detach_array_buffer(vm, array_buffer_object, vm.argument(1)));
    return JS::js_null();
}

TESTJS_GLOBAL_FUNCTION(set_time_zone, setTimeZone)
{
    auto current_time_zone = JS::PrimitiveString::create(vm, Unicode::current_time_zone());
    auto time_zone = TRY(vm.argument(0).to_string(vm));

    if (auto result = Unicode::set_current_time_zone(time_zone); result.is_error())
        return vm.throw_completion<JS::InternalError>(MUST(String::formatted("Could not set time zone: {}", result.error())));

    JS::clear_system_time_zone_cache();
    return current_time_zone;
}

TESTJS_GLOBAL_FUNCTION(to_utf8_bytes, toUTF8Bytes)
{
    auto& realm = *vm.current_realm();

    auto string = TRY(vm.argument(0).to_string(vm));
    auto typed_array = TRY(JS::Uint8Array::create(realm, string.bytes().size()));

    for (auto [i, byte] : enumerate(string.bytes()))
        typed_array->set_value_in_buffer(i, JS::Value { byte }, JS::ArrayBuffer::Order::SeqCst);

    return typed_array;
}

TESTJS_RUN_FILE_FUNCTION(ByteString const& test_file, JS::Realm& realm, JS::ExecutionContext&)
{
    if (!test262_parser_tests)
        return Test::JS::RunFileHookResult::RunAsNormal;

    auto start_time = Test::get_time_in_ms();

    LexicalPath path(test_file);
    auto dirname = path.dirname();
    enum {
        Early,
        Fail,
        Pass,
        ExplicitPass,
    } expectation { Pass };

    if (dirname.ends_with("early"sv))
        expectation = Early;
    else if (dirname.ends_with("fail"sv))
        expectation = Fail;
    else if (dirname.ends_with("pass-explicit"sv))
        expectation = ExplicitPass;
    else if (dirname.ends_with("pass"sv))
        expectation = Pass;
    else
        return Test::JS::RunFileHookResult::SkipFile;

    auto program_type = path.basename().ends_with(".module.js"sv) ? JS::Program::Type::Module : JS::Program::Type::Script;
    bool parse_succeeded = false;
    if (program_type == JS::Program::Type::Module)
        parse_succeeded = !Test::JS::parse_module(test_file, realm).is_error();
    else
        parse_succeeded = !Test::JS::parse_script(test_file, realm).is_error();

    bool test_passed = true;
    String message;
    String expectation_string;

    switch (expectation) {
    case Early:
    case Fail:
        expectation_string = "File should not parse"_string;
        test_passed = !parse_succeeded;
        if (!test_passed)
            message = "Expected the file to fail parsing, but it did not"_string;
        break;
    case Pass:
    case ExplicitPass:
        expectation_string = "File should parse"_string;
        test_passed = parse_succeeded;
        if (!test_passed)
            message = "Expected the file to parse, but it did not"_string;
        break;
    }

    auto test_result = test_passed ? Test::Result::Pass : Test::Result::Fail;
    auto test_path = *LexicalPath::relative_path(test_file, Test::JS::g_test_root);
    auto duration_ms = Test::get_time_in_ms() - start_time;
    return Test::JS::JSFileResult {
        test_path,
        {},
        duration_ms,
        test_result,
        { Test::Suite { test_path, "Parse file"_string, test_result, { { move(expectation_string), test_result, move(message), static_cast<u64>(duration_ms) * 1000u } } } }
    };
}
