/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Result.h>
#include <AK/Tuple.h>
#include <LibCore/DirIterator.h>
#include <LibCore/File.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/WeakMap.h>
#include <LibJS/Runtime/WeakSet.h>
#include <LibJS/Script.h>
#include <LibJS/SourceTextModule.h>
#include <LibTest/Results.h>
#include <LibTest/TestRunner.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#define STRCAT(x, y) __STRCAT(x, y)
#define STRSTRCAT(x, y) __STRSTRCAT(x, y)
#define __STRCAT(x, y) x #y
#define __STRSTRCAT(x, y) x y

// Note: This is a little weird, so here's an explanation:
//       If the vararg isn't given, the tuple initializer will simply expand to `fn, ::Test::JS::__testjs_last<1>()`
//       and if it _is_ given (say as `A`), the tuple initializer will expand to `fn, ::Test::JS::__testjs_last<1, A>()`, which will end up being evaluated as `A`
//       and if multiple args are given, the static_assert will be sad.
#define __TESTJS_REGISTER_GLOBAL_FUNCTION(name, fn, ...)                                                                                        \
    struct __TestJS_register_##fn {                                                                                                             \
        static_assert(                                                                                                                          \
            ::Test::JS::__testjs_count(__VA_ARGS__) <= 1,                                                                                       \
            STRCAT(STRSTRCAT(STRCAT("Expected at most three arguments to TESTJS_GLOBAL_FUNCTION at line", __LINE__), ", in file "), __FILE__)); \
        __TestJS_register_##fn() noexcept                                                                                                       \
        {                                                                                                                                       \
            ::Test::JS::s_exposed_global_functions.set(                                                                                         \
                name,                                                                                                                           \
                { fn, ::Test::JS::__testjs_last<1, ##__VA_ARGS__>() });                                                                         \
        }                                                                                                                                       \
    } __testjs_register_##fn {};

#define TESTJS_GLOBAL_FUNCTION(function, exposed_name, ...)                             \
    JS_DECLARE_NATIVE_FUNCTION(function);                                               \
    __TESTJS_REGISTER_GLOBAL_FUNCTION(#exposed_name##_string, function, ##__VA_ARGS__); \
    JS_DEFINE_NATIVE_FUNCTION(function)

#define TESTJS_MAIN_HOOK()                  \
    struct __TestJS_main_hook {             \
        __TestJS_main_hook()                \
        {                                   \
            ::Test::JS::g_main_hook = hook; \
        }                                   \
        static void hook();                 \
    } __testjs_common_register_##name {};   \
    void __TestJS_main_hook::hook()

#define TESTJS_PROGRAM_FLAG(flag, help_string, long_name, short_name)                      \
    bool flag { false };                                                                   \
    struct __TestJS_flag_hook_##flag {                                                     \
        __TestJS_flag_hook_##flag()                                                        \
        {                                                                                  \
            ::Test::JS::g_extra_args.set(&(flag), { help_string, long_name, short_name }); \
        }                                                                                  \
    } __testjs_flag_hook_##flag;

#define TEST_ROOT(path) \
    ByteString Test::JS::g_test_root_fragment = path

#define TESTJS_RUN_FILE_FUNCTION(...)                                                                            \
    struct __TestJS_run_file {                                                                                   \
        __TestJS_run_file()                                                                                      \
        {                                                                                                        \
            ::Test::JS::g_run_file = hook;                                                                       \
        }                                                                                                        \
        static ::Test::JS::IntermediateRunFileResult hook(ByteString const&, JS::Realm&, JS::ExecutionContext&); \
    } __testjs_common_run_file {};                                                                               \
    ::Test::JS::IntermediateRunFileResult __TestJS_run_file::hook(__VA_ARGS__)

namespace Test::JS {

namespace JS = ::JS;

template<typename... Args>
static consteval size_t __testjs_count(Args...) { return sizeof...(Args); }

template<auto... Values>
static consteval size_t __testjs_last()
{
    Array values { Values... };
    return values[values.size() - 1U];
}

static constexpr auto TOP_LEVEL_TEST_NAME = "__$$TOP_LEVEL$$__";
extern RefPtr<JS::VM> g_vm;
extern bool g_collect_on_every_allocation;
extern ByteString g_currently_running_test;
struct FunctionWithLength {
    JS::ThrowCompletionOr<JS::Value> (*function)(JS::VM&);
    size_t length { 0 };
};
extern HashMap<String, FunctionWithLength> s_exposed_global_functions;
extern ByteString g_test_root_fragment;
extern ByteString g_test_root;
extern int g_test_argc;
extern char** g_test_argv;
extern Function<void()> g_main_hook;
extern HashMap<bool*, Tuple<ByteString, ByteString, char>> g_extra_args;

struct ParserError {
    JS::ParserError error;
    ByteString hint;
};

struct JSFileResult {
    ByteString name;
    Optional<ParserError> error {};
    double time_taken { 0 };
    // A failed test takes precedence over a skipped test, which both have
    // precedence over a passed test
    Test::Result most_severe_test_result { Test::Result::Pass };
    Vector<Test::Suite> suites {};
    Vector<ByteString> logged_messages {};
};

enum class RunFileHookResult {
    RunAsNormal,
    SkipFile,
};

using IntermediateRunFileResult = AK::Result<JSFileResult, RunFileHookResult>;
extern IntermediateRunFileResult (*g_run_file)(ByteString const&, JS::Realm&, JS::ExecutionContext&);

class TestRunner : public ::Test::TestRunner {
public:
    TestRunner(ByteString test_root, ByteString common_path, bool print_times, bool print_progress, bool print_json, bool detailed_json)
        : ::Test::TestRunner(move(test_root), print_times, print_progress, print_json, detailed_json)
        , m_common_path(move(common_path))
    {
        g_test_root = m_test_root;
    }

    virtual ~TestRunner() = default;

protected:
    virtual void do_run_single_test(ByteString const& test_path, size_t, size_t) override;
    virtual Vector<ByteString> get_test_paths() const override;
    virtual JSFileResult run_file_test(ByteString const& test_path);
    void print_file_result(JSFileResult const& file_result) const;

    ByteString m_common_path;
};

class TestRunnerGlobalObject final : public JS::GlobalObject {
    JS_OBJECT(TestRunnerGlobalObject, JS::GlobalObject);

public:
    TestRunnerGlobalObject(JS::Realm& realm)
        : JS::GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~TestRunnerGlobalObject() override = default;
};

inline void TestRunnerGlobalObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global"_fly_string, this, JS::Attribute::Enumerable);
    for (auto& entry : s_exposed_global_functions) {
        define_native_function(
            realm,
            entry.key, [fn = entry.value.function](auto& vm) {
                return fn(vm);
            },
            entry.value.length, JS::default_attributes);
    }
}

inline ByteBuffer load_entire_file(StringView path)
{
    auto try_load_entire_file = [](StringView const& path) -> ErrorOr<ByteBuffer> {
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
        auto file_size = TRY(file->size());
        auto content = TRY(ByteBuffer::create_uninitialized(file_size));
        TRY(file->read_until_filled(content.bytes()));
        return content;
    };

    auto buffer_or_error = try_load_entire_file(path);
    if (buffer_or_error.is_error()) {
        warnln("Failed to open the following file: \"{}\", error: {}", path, buffer_or_error.release_error());
        cleanup_and_exit();
    }
    return buffer_or_error.release_value();
}

inline AK::Result<GC::Ref<JS::Script>, ParserError> parse_script(StringView path, JS::Realm& realm)
{
    auto contents = load_entire_file(path);
    auto script_or_errors = JS::Script::parse(contents, realm, path);

    if (script_or_errors.is_error()) {
        auto errors = script_or_errors.release_error();
        return ParserError { errors[0], errors[0].source_location_hint(contents) };
    }

    return script_or_errors.release_value();
}

inline AK::Result<GC::Ref<JS::SourceTextModule>, ParserError> parse_module(StringView path, JS::Realm& realm)
{
    auto contents = load_entire_file(path);
    auto script_or_errors = JS::SourceTextModule::parse(contents, realm, path);

    if (script_or_errors.is_error()) {
        auto errors = script_or_errors.release_error();
        return ParserError { errors[0], errors[0].source_location_hint(contents) };
    }

    return script_or_errors.release_value();
}

inline ErrorOr<JsonValue> get_test_results(JS::Realm& realm)
{
    auto results = MUST(realm.global_object().get("__TestResults__"_fly_string));
    auto maybe_json_string = MUST(JS::JSONObject::stringify_impl(*g_vm, results, JS::js_undefined(), JS::js_undefined()));
    if (maybe_json_string.has_value())
        return JsonValue::from_string(*maybe_json_string);
    return JsonValue();
}

inline void TestRunner::do_run_single_test(ByteString const& test_path, size_t, size_t)
{
    auto file_result = run_file_test(test_path);
    if (!m_print_json)
        print_file_result(file_result);

    if (needs_detailed_suites())
        ensure_suites().extend(file_result.suites);
}

inline Vector<ByteString> TestRunner::get_test_paths() const
{
    Vector<ByteString> paths;
    iterate_directory_recursively(m_test_root, [&](ByteString const& file_path) {
        if (!file_path.ends_with(".js"sv))
            return;
        if (!file_path.ends_with("test-common.js"sv))
            paths.append(file_path);
    });
    quick_sort(paths);
    return paths;
}

inline JSFileResult TestRunner::run_file_test(ByteString const& test_path)
{
    g_currently_running_test = test_path;

    double start_time = get_time_in_ms();

    GC::Ptr<JS::Realm> realm;
    GC::Ptr<TestRunnerGlobalObject> global_object;
    auto root_execution_context = MUST(JS::Realm::initialize_host_defined_realm(
        *g_vm,
        [&](JS::Realm& realm_) -> JS::GlobalObject* {
            realm = &realm_;
            global_object = realm->create<TestRunnerGlobalObject>(*realm);
            return global_object;
        },
        nullptr));
    auto& global_execution_context = *root_execution_context;
    g_vm->pop_execution_context();

    g_vm->heap().set_should_collect_on_every_allocation(g_collect_on_every_allocation);

    if (g_run_file) {
        auto result = g_run_file(test_path, *realm, global_execution_context);
        if (result.is_error() && result.error() == RunFileHookResult::SkipFile) {
            return {
                test_path,
                {},
                0,
                Test::Result::Skip,
                {},
                {}
            };
        }
        if (!result.is_error()) {
            auto value = result.release_value();
            for (auto& suite : value.suites) {
                if (suite.most_severe_test_result == Result::Pass)
                    m_counts.suites_passed++;
                else if (suite.most_severe_test_result == Result::Fail)
                    m_counts.suites_failed++;
                for (auto& test : suite.tests) {
                    if (test.result == Result::Pass)
                        m_counts.tests_passed++;
                    else if (test.result == Result::Fail)
                        m_counts.tests_failed++;
                    else if (test.result == Result::Skip)
                        m_counts.tests_skipped++;
                }
            }
            ++m_counts.files_total;
            m_total_elapsed_time_in_ms += value.time_taken;

            return value;
        }
    }

    // FIXME: Since a new realm is created every time, we no longer cache the test-common.js file as scripts are parsed for the current realm only.
    //        Find a way to cache this.
    auto result = parse_script(m_common_path, *realm);
    if (result.is_error()) {
        warnln("Unable to parse test-common.js");
        warnln("{}", result.error().error.to_byte_string());
        warnln("{}", result.error().hint);
        cleanup_and_exit();
    }
    auto test_script = result.release_value();

    g_vm->push_execution_context(global_execution_context);
    MUST(g_vm->bytecode_interpreter().run(*test_script));
    g_vm->pop_execution_context();

    auto file_script = parse_script(test_path, *realm);
    JS::ThrowCompletionOr<JS::Value> top_level_result { JS::js_undefined() };
    if (file_script.is_error())
        return { test_path, file_script.error() };
    g_vm->push_execution_context(global_execution_context);
    top_level_result = g_vm->bytecode_interpreter().run(file_script.value());
    g_vm->pop_execution_context();

    g_vm->push_execution_context(global_execution_context);
    auto test_json = get_test_results(*realm);
    g_vm->pop_execution_context();
    if (test_json.is_error()) {
        warnln("Received malformed JSON from test \"{}\"", test_path);
        cleanup_and_exit();
    }

    JSFileResult file_result { test_path.substring(m_test_root.length() + 1, test_path.length() - m_test_root.length() - 1) };

    // Collect logged messages
    auto user_output = MUST(realm->global_object().get("__UserOutput__"_fly_string));

    auto& arr = user_output.as_array();
    for (auto& entry : arr.indexed_properties()) {
        auto message = MUST(arr.get(entry.index()));
        file_result.logged_messages.append(message.to_string_without_side_effects().to_byte_string());
    }

    test_json.value().as_object().for_each_member([&](String const& suite_name, JsonValue const& suite_value) {
        Test::Suite suite { test_path, suite_name };

        VERIFY(suite_value.is_object());

        suite_value.as_object().for_each_member([&](String const& test_name, JsonValue const& test_value) {
            Test::Case test { test_name, Test::Result::Fail, {}, 0 };

            VERIFY(test_value.is_object());
            VERIFY(test_value.as_object().has("result"sv));

            auto result = test_value.as_object().get_string("result"sv);
            VERIFY(result.has_value());
            auto result_string = result.value();
            if (result_string == "pass") {
                test.result = Test::Result::Pass;
                m_counts.tests_passed++;
            } else if (result_string == "fail") {
                test.result = Test::Result::Fail;
                m_counts.tests_failed++;
                suite.most_severe_test_result = Test::Result::Fail;
                VERIFY(test_value.as_object().has("details"sv));
                auto details = test_value.as_object().get_string("details"sv);
                VERIFY(result.has_value());
                test.details = details.release_value();
            } else if (result_string == "xfail") {
                test.result = Test::Result::ExpectedFail;
                m_counts.tests_expected_failed++;
                if (suite.most_severe_test_result != Test::Result::Fail)
                    suite.most_severe_test_result = Test::Result::ExpectedFail;
            } else {
                test.result = Test::Result::Skip;
                if (suite.most_severe_test_result == Test::Result::Pass)
                    suite.most_severe_test_result = Test::Result::Skip;
                m_counts.tests_skipped++;
            }

            test.duration_us = test_value.as_object().get_u64("duration"sv).value_or(0);

            suite.tests.append(test);
        });

        if (suite.most_severe_test_result == Test::Result::Fail) {
            m_counts.suites_failed++;
            file_result.most_severe_test_result = Test::Result::Fail;
        } else {
            if (suite.most_severe_test_result == Test::Result::Skip && file_result.most_severe_test_result == Test::Result::Pass)
                file_result.most_severe_test_result = Test::Result::Skip;
            else if (suite.most_severe_test_result == Test::Result::ExpectedFail && (file_result.most_severe_test_result == Test::Result::Pass || file_result.most_severe_test_result == Test::Result::Skip))
                file_result.most_severe_test_result = Test::Result::ExpectedFail;
            m_counts.suites_passed++;
        }

        file_result.suites.append(suite);
    });

    if (top_level_result.is_error()) {
        Test::Suite suite { test_path, "<top-level>"_string };
        suite.most_severe_test_result = Result::Crashed;

        Test::Case test_case { "<top-level>"_string, Test::Result::Fail, {}, 0 };
        auto error = top_level_result.release_error().release_value().release_value();
        if (error.is_object()) {
            StringBuilder detail_builder;

            auto& error_object = error.as_object();
            auto name = error_object.get_without_side_effects(g_vm->names.name).value_or(JS::js_undefined());
            auto message = error_object.get_without_side_effects(g_vm->names.message).value_or(JS::js_undefined());

            if (name.is_accessor() || message.is_accessor()) {
                detail_builder.append(error.to_string_without_side_effects());
            } else {
                detail_builder.append(name.to_string_without_side_effects());
                detail_builder.append(": "sv);
                detail_builder.append(message.to_string_without_side_effects());
            }

            if (is<JS::Error>(error_object)) {
                auto& error_as_error = static_cast<JS::Error&>(error_object);
                detail_builder.append('\n');
                detail_builder.append(error_as_error.stack_string());
            }

            test_case.details = MUST(detail_builder.to_string());
        } else {
            test_case.details = error.to_string_without_side_effects();
        }

        suite.tests.append(move(test_case));

        file_result.suites.append(suite);

        m_counts.suites_failed++;
        file_result.most_severe_test_result = Test::Result::Fail;
    }

    m_counts.files_total++;

    file_result.time_taken = get_time_in_ms() - start_time;
    m_total_elapsed_time_in_ms += file_result.time_taken;

    return file_result;
}

inline void TestRunner::print_file_result(JSFileResult const& file_result) const
{
    if (file_result.most_severe_test_result == Test::Result::Fail || file_result.error.has_value()) {
        print_modifiers({ BG_RED, FG_BOLD });
        out(" FAIL ");
        print_modifiers({ CLEAR });
    } else {
        if (m_print_times || file_result.most_severe_test_result != Test::Result::Pass) {
            print_modifiers({ BG_GREEN, FG_BLACK, FG_BOLD });
            out(" PASS ");
            print_modifiers({ CLEAR });
        } else {
            return;
        }
    }

    out(" {}", file_result.name);

    if (m_print_times) {
        print_modifiers({ CLEAR, ITALIC, FG_GRAY });
        if (file_result.time_taken < 1000) {
            outln(" ({}ms)", static_cast<int>(file_result.time_taken));
        } else {
            outln(" ({:3}s)", file_result.time_taken / 1000.0);
        }
        print_modifiers({ CLEAR });
    } else {
        outln();
    }

    if (!file_result.logged_messages.is_empty()) {
        print_modifiers({ FG_GRAY, FG_BOLD });
        outln("    ℹ️  Console output:");
        print_modifiers({ CLEAR, FG_GRAY });
        for (auto& message : file_result.logged_messages)
            outln("         {}", message);
    }

    if (file_result.error.has_value()) {
        auto test_error = file_result.error.value();

        print_modifiers({ FG_RED });
        outln("    ❌ The file failed to parse");
        outln();
        print_modifiers({ FG_GRAY });
        for (auto& message : test_error.hint.split('\n', SplitBehavior::KeepEmpty)) {
            outln("         {}", message);
        }
        print_modifiers({ FG_RED });
        outln("         {}", test_error.error.to_byte_string());
        outln();
        return;
    }

    if (file_result.most_severe_test_result != Test::Result::Pass) {
        for (auto& suite : file_result.suites) {
            if (suite.most_severe_test_result == Test::Result::Pass)
                continue;

            bool failed = suite.most_severe_test_result == Test::Result::Fail;

            print_modifiers({ FG_GRAY, FG_BOLD });

            if (failed) {
                out("    ❌ Suite:  ");
            } else {
                out("    ⚠️  Suite:  ");
            }

            print_modifiers({ CLEAR, FG_GRAY });

            if (suite.name == TOP_LEVEL_TEST_NAME) {
                outln("<top-level>");
            } else {
                outln("{}", suite.name);
            }
            print_modifiers({ CLEAR });

            for (auto& test : suite.tests) {
                if (test.result == Test::Result::Pass)
                    continue;

                print_modifiers({ FG_GRAY, FG_BOLD });
                out("         Test:   ");
                if (test.result == Test::Result::Fail) {
                    print_modifiers({ CLEAR, FG_RED });
                    outln("{} (failed):", test.name);
                    outln("                 {}", test.details);
                } else if (test.result == Test::Result::ExpectedFail) {
                    print_modifiers({ CLEAR, FG_ORANGE });
                    outln("{} (expected fail)", test.name);
                } else {
                    print_modifiers({ CLEAR, FG_ORANGE });
                    outln("{} (skipped)", test.name);
                }
                print_modifiers({ CLEAR });
            }
        }
    }
}

}
