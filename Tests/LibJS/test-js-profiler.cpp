/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/Platform.h>
#include <AK/Utf16String.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/GeckoProfileWriter.h>
#include <LibJS/Profiler.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>

static constexpr StringView k_default_filename = "profiler-test.js"sv;

static void run_js(JS::VM& vm, JS::Realm& realm, StringView source, StringView filename = k_default_filename)
{
    auto script = JS::Script::parse(source, realm, filename);
    VERIFY(!script.is_error());
    auto result = vm.bytecode_interpreter().run(*script.value());
    VERIFY(!result.is_error());
}

static void install_request_sample_function(JS::Realm& realm, JS::Profiler& profiler)
{
    realm.global_object().define_native_function(
        realm,
        Utf16FlyString::from_utf8("requestSample"sv),
        [&profiler](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
            profiler.request_sample_for_test();
            return JS::js_undefined();
        },
        0,
        JS::Attribute::Writable | JS::Attribute::Configurable);
}

static bool string_table_contains(JS::Profiler const& profiler, StringView needle)
{
    for (auto const& str : profiler.string_table()) {
        if (str.contains(needle))
            return true;
    }
    return false;
}

static Vector<String> sample_frame_locations(JS::Profiler const& profiler, size_t sample_index)
{
    Vector<String> locations;
    Optional<u32> stack_index = profiler.samples()[sample_index].stack_index;
    while (stack_index.has_value()) {
        auto const& stack = profiler.stack_table()[*stack_index];
        auto const& frame = profiler.frame_table()[stack.frame_index];
        locations.append(profiler.string_table()[frame.string_index]);
        stack_index = stack.prefix;
    }
    return locations;
}

static Optional<Vector<String>> find_sample_with_leaf(JS::Profiler const& profiler, StringView leaf_name)
{
    for (size_t i = 0; i < profiler.samples().size(); ++i) {
        auto locations = sample_frame_locations(profiler, i);
        if (!locations.is_empty() && locations[0].contains(leaf_name))
            return locations;
    }
    return {};
}

// Run JS long enough that timer-driven async samplers capture multiple samples.
// fib(30) ≈ 1.3 M recursive calls, reliably runs for tens of milliseconds.
static constexpr StringView k_workload = R"js(
function fib(n) { return n <= 1 ? n : fib(n - 1) + fib(n - 2); }
fib(30);
)js"sv;

TEST_CASE(profiler_collects_samples)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 1000 /* 1 ms */);
    if (!profiler.supports_timed_sampling())
        return;

    vm->set_profiler(&profiler);
    profiler.start();
    run_js(*vm, realm, k_workload);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT(profiler.samples().size() > 0u);
    EXPECT(string_table_contains(profiler, "fib"sv));
}

TEST_CASE(profiler_samples_global_scope_bytecode)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 0);
    vm->set_profiler(&profiler);
    install_request_sample_function(realm, profiler);
    profiler.start();
    // requestSample() sets the pending flag; the sample is then captured at the
    // next bytecode dispatch boundary inside the loop, which runs in global scope.
    run_js(*vm, realm, R"js(
requestSample();
let sum = 0;
for (let i = 0; i < 500000; i++)
    sum += i;
)js"sv,
        "profiler-global.js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT(profiler.samples().size() > 0u);
    EXPECT(string_table_contains(profiler, "profiler-global.js"sv));
}

TEST_CASE(profiler_safe_point_sampling_uses_current_frame)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 0);
    vm->set_profiler(&profiler);
    install_request_sample_function(realm, profiler);
    profiler.start();
    run_js(*vm, realm, R"js(
function inner() {
    requestSample();
    let sum = 0;
    for (let i = 0; i < 500000; i++)
        sum += i;
    return sum;
}
function outer() {
    return inner();
}
outer();
)js"sv,
        "profiler-safe-point.js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT(profiler.samples().size() > 0u);
    auto sample_locations = find_sample_with_leaf(profiler, "inner"sv);
    VERIFY(sample_locations.has_value());
    EXPECT(sample_locations->size() >= 2u);
    EXPECT((*sample_locations)[0].contains("inner (profiler-safe-point.js:"sv));
    EXPECT((*sample_locations)[1].contains("outer (profiler-safe-point.js:"sv));
}

TEST_CASE(profiler_gecko_json_is_valid)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 0);
    vm->set_profiler(&profiler);
    install_request_sample_function(realm, profiler);
    profiler.start();
    run_js(*vm, realm, R"js(
requestSample();
let sum = 0;
for (let i = 0; i < 500000; i++)
    sum += i;
)js"sv,
        "profiler-gecko.js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT(profiler.samples().size() > 0u);

    auto json_string = JS::write_gecko_profile(profiler);
    EXPECT(!json_string.is_empty());

    auto parsed = JsonParser::parse(json_string);
    EXPECT(!parsed.is_error());
    EXPECT(parsed.value().is_object());

    auto const& root = parsed.value().as_object();
    EXPECT(root.has("meta"sv));
    EXPECT(root.has("threads"sv));
    EXPECT(root.get("threads"sv)->is_array());
    EXPECT_EQ(root.get("threads"sv)->as_array().size(), 1u);

    auto const& thread = root.get("threads"sv)->as_array()[0].as_object();
    EXPECT(thread.has("samples"sv));
    EXPECT(thread.has("frameTable"sv));
    EXPECT(thread.has("stackTable"sv));
    EXPECT(thread.has("stringTable"sv));
    EXPECT(thread.get("stringTable"sv)->as_array().size() > 0u);
}

TEST_CASE(profiler_timed_sampling_captures_call_stack)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 1000 /* 1 ms */);
    if (!profiler.supports_timed_sampling())
        return;

    vm->set_profiler(&profiler);
    profiler.start();
    run_js(*vm, realm, R"js(
function alpha() {
    let s = 0;
    for (let i = 0; i < 5000000; i++) s += i;
    return s;
}
function beta() { return alpha(); }
beta();
)js"sv,
        "profiler-timed-stack.js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    // Verify we captured samples with a multi-frame stack through the async path.
    auto sample_locations = find_sample_with_leaf(profiler, "alpha"sv);
    VERIFY(sample_locations.has_value());
    EXPECT(sample_locations->size() >= 2u);
    EXPECT((*sample_locations)[0].contains("alpha (profiler-timed-stack.js:"sv));
    EXPECT((*sample_locations)[1].contains("beta (profiler-timed-stack.js:"sv));
}

TEST_CASE(profiler_captures_anonymous_functions)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 0);
    vm->set_profiler(&profiler);
    install_request_sample_function(realm, profiler);
    profiler.start();
    run_js(*vm, realm, R"js(
(function() {
    requestSample();
    let sum = 0;
    for (let i = 0; i < 500000; i++) sum += i;
})();
)js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT(profiler.samples().size() > 0u);
    EXPECT(string_table_contains(profiler, "(anonymous)"sv));
}

TEST_CASE(profiler_uses_inferred_member_assignment_name)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 0);
    vm->set_profiler(&profiler);
    install_request_sample_function(realm, profiler);
    profiler.start();
    run_js(*vm, realm, R"js(
let object = {};
object.method = function() {
    requestSample();
    let sum = 0;
    for (let i = 0; i < 500000; i++) sum += i;
    return sum;
};
object.method();
)js"sv,
        "profiler-display-name.js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT_EQ(profiler.samples().size(), 1u);
    EXPECT(string_table_contains(profiler, "object.method (profiler-display-name.js:"sv));
}

TEST_CASE(profiler_can_be_reused)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    JS::Profiler profiler(*vm, 0);
    vm->set_profiler(&profiler);
    install_request_sample_function(realm, profiler);

    profiler.start();
    run_js(*vm, realm, R"js(
(function() {
    requestSample();
    let sum = 0;
    for (let i = 0; i < 500000; i++) sum += i;
})();
)js"sv,
        "profiler-reuse-first.js"sv);
    profiler.stop();
    EXPECT_EQ(profiler.samples().size(), 1u);
    EXPECT(string_table_contains(profiler, "profiler-reuse-first.js"sv));

    profiler.start();
    run_js(*vm, realm, R"js(
(function() {
    requestSample();
    let sum = 0;
    for (let i = 0; i < 500000; i++) sum += i;
})();
)js"sv,
        "profiler-reuse-second.js"sv);
    profiler.stop();
    vm->set_profiler(nullptr);

    EXPECT_EQ(profiler.samples().size(), 1u);
    EXPECT(string_table_contains(profiler, "profiler-reuse-second.js"sv));
    EXPECT(!string_table_contains(profiler, "profiler-reuse-first.js"sv));
}

TEST_CASE(profiler_stop_without_start_is_safe)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);

    // stop() on a profiler that was never started should not crash.
    JS::Profiler profiler(*vm);
    profiler.stop();
    EXPECT_EQ(profiler.samples().size(), 0u);
}
