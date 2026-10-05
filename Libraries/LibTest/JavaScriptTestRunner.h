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
#include <LibGC/Root.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/Script.h>
#include <LibJS/SourceTextModule.h>
#include <LibTest/JavaScriptTestHost.h>
#include <LibTest/Results.h>
#include <LibTest/TestRunner.h>

namespace Test::JS {

static constexpr auto TOP_LEVEL_TEST_NAME = "__$$TOP_LEVEL$$__";
extern RefPtr<JS::VM> g_vm;
extern bool g_collect_on_every_allocation;
extern ByteString g_currently_running_test;
extern ByteString g_test_root;
extern int g_test_argc;
extern char** g_test_argv;

class TestRunner : public ::Test::TestRunner {
public:
    TestRunner(ByteString test_root, ByteString common_path, bool print_times, bool print_progress, bool print_json, bool detailed_json, bool print_each_test)
        : ::Test::TestRunner(move(test_root), print_times, print_progress, print_json, detailed_json, print_each_test)
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
    auto source_text = Utf16String::from_utf8(StringView { contents.bytes() });
    auto display_filename = Utf16String::from_utf8(path);
    auto script_or_errors = JS::Script::parse(source_text.utf16_view(), realm, path, display_filename.utf16_view());

    if (script_or_errors.is_error()) {
        auto errors = script_or_errors.release_error();
        return ParserError { errors[0], errors[0].source_location_hint(source_text) };
    }

    return script_or_errors.release_value();
}

inline AK::Result<GC::Ref<JS::SourceTextModule>, ParserError> parse_module(StringView path, JS::Realm& realm)
{
    auto contents = load_entire_file(path);
    auto source_text = Utf16String::from_utf8(StringView { contents.bytes() });
    auto display_filename = Utf16String::from_utf8(path);
    auto script_or_errors = JS::SourceTextModule::parse(source_text.utf16_view(), realm, path, display_filename.utf16_view());

    if (script_or_errors.is_error()) {
        auto errors = script_or_errors.release_error();
        return ParserError { errors[0], errors[0].source_location_hint(source_text) };
    }

    return script_or_errors.release_value();
}

inline ErrorOr<JsonValue> get_test_results(JS::Realm& realm)
{
    auto results = MUST(realm.global_object().get("__TestResults__"_utf16_fly_string));
    auto maybe_json_string = MUST(JS::JSONObject::stringify_impl(*g_vm, results, JS::js_undefined(), JS::js_undefined()));
    if (maybe_json_string.has_value())
        return JsonValue::from_string(MUST(maybe_json_string->utf16_view().to_utf8()));
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

inline void print_test_timings(String test_name, JS::Value state_value)
{
    if (state_value.is_string()) {
        auto state_string = state_value.as_string().utf16_string_view().to_utf8_but_should_be_ported_to_utf16();
        if (state_string == "pass"sv) {
            print_modifiers({ FG_BOLD });
            out("Finished: ");
            print_modifiers({ CLEAR });
            outln("{} (PASS)", test_name);
        } else if (state_string == "fail"sv) {
            print_modifiers({ FG_RED, FG_BOLD });
            out("Finished: ");
            print_modifiers({ CLEAR });
            outln("{} (FAIL)", test_name);
        } else if (state_string == "xfail"sv) {
            print_modifiers({ FG_ORANGE, FG_BOLD });
            out("Finished: ");
            print_modifiers({ CLEAR });
            outln("{} (XFAIL)", test_name);
        } else if (state_string == "start"sv) {
            print_modifiers({ BG_GREEN, FG_ORANGE });
            out("Running: ");
            print_modifiers({ CLEAR });
            outln("{}", test_name);
        }
    }
}

inline void define_test_runner_globals(JS::Realm& realm, bool prints_test_timings)
{
    auto& global_object = realm.global_object();
    global_object.define_direct_property("global"_utf16_fly_string, &global_object, JS::Attribute::Enumerable);
    global_object.define_native_function(
        realm,
        "__reportTest__"_utf16,
        [prints_test_timings](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!prints_test_timings)
                return JS::js_undefined();
            auto test_name = TRY(vm.argument(0).to_utf16_string(vm)).to_utf8_but_should_be_ported_to_utf16();
            print_test_timings(test_name, vm.argument(1));
            return JS::js_undefined();
        },
        2, JS::default_attributes);
    for (auto& entry : s_exposed_global_functions) {
        global_object.define_native_function(
            realm,
            entry.key,
            [fn = entry.value.function](auto& vm) {
                return fn(vm);
            },
            entry.value.length, JS::default_attributes);
    }
}

inline JSFileResult TestRunner::run_file_test(ByteString const& test_path)
{
    g_currently_running_test = test_path;

    double start_time = get_time_in_ms();

    auto root_execution_context = MUST(JS::Realm::initialize_host_defined_realm(*g_vm, nullptr, nullptr));
    auto& global_execution_context = *root_execution_context;
    // Nothing else keeps the realm alive while its execution context is off the stack.
    auto realm = GC::make_root(*global_execution_context.realm);
    define_test_runner_globals(*realm, needs_timings());
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
        warnln("{}", result.error().error.to_utf16_string().to_byte_string());
        warnln("{}", result.error().hint);
        cleanup_and_exit();
    }
    auto test_script = result.release_value();

    g_vm->push_execution_context(global_execution_context);
    MUST(g_vm->run(*test_script));
    g_vm->pop_execution_context();

    auto file_script = parse_script(test_path, *realm);
    JS::ThrowCompletionOr<JS::Value> top_level_result { JS::js_undefined() };
    if (file_script.is_error()) {
        m_counts.suites_failed++;
        m_counts.files_total++;
        return { test_path, file_script.error() };
    }
    g_vm->push_execution_context(global_execution_context);
    top_level_result = g_vm->run(file_script.value());
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
    auto user_output = MUST(realm->global_object().get("__UserOutput__"_utf16_fly_string));

    auto& user_output_array = as<JS::Array>(user_output.as_object());
    for (u32 i = 0; i < user_output_array.indexed_array_like_size(); ++i) {
        auto message = MUST(user_output_array.get(i));
        file_result.logged_messages.append(message.to_utf16_string_without_side_effects().to_utf8().to_byte_string());
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
        auto error = top_level_result.release_error().release_value();
        if (error.is_object()) {
            StringBuilder detail_builder;

            auto& error_object = error.as_object();
            auto name = error_object.get_without_side_effects(g_vm->names.name);
            auto message = error_object.get_without_side_effects(g_vm->names.message);

            if (name.is_accessor() || message.is_accessor()) {
                auto error_string = error.to_utf16_string_without_side_effects();
                detail_builder.append(error_string.utf16_view());
            } else {
                auto name_string = name.to_utf16_string_without_side_effects();
                detail_builder.append(name_string.utf16_view());
                detail_builder.append(": "sv);
                auto message_string = message.to_utf16_string_without_side_effects();
                detail_builder.append(message_string.utf16_view());
            }

            if (auto const* error_as_error = as_if<JS::Error>(error_object)) {
                detail_builder.append('\n');
                detail_builder.append(error_as_error->stack_string());
            }

            test_case.details = MUST(detail_builder.to_string());
        } else {
            test_case.details = error.to_utf16_string_without_side_effects().to_utf8();
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
        for (auto& message : test_error.hint.split_view('\n', SplitBehavior::KeepEmpty)) {
            outln("         {}", message);
        }
        print_modifiers({ FG_RED });
        outln("         {}", test_error.error.to_utf16_string().to_byte_string());
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
