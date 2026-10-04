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

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Result.h>
#include <AK/Tuple.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Completion.h>
#include <LibTest/Results.h>

// What a JavaScript test program registers with the runner of its test files: the global functions its tests call,
// its command line flags, and hooks into the run. Nothing here depends on the runner that consumes the registrations.

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

#define TESTJS_GLOBAL_FUNCTION(function, exposed_name, ...)                            \
    JS_DECLARE_NATIVE_FUNCTION(function);                                              \
    __TESTJS_REGISTER_GLOBAL_FUNCTION(#exposed_name##_utf16, function, ##__VA_ARGS__); \
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

struct FunctionWithLength {
    JS::ThrowCompletionOr<JS::Value> (*function)(JS::VM&);
    size_t length { 0 };
};
extern HashMap<Utf16String, FunctionWithLength> s_exposed_global_functions;
extern ByteString g_test_root_fragment;
extern void (*g_main_hook)();
extern HashMap<bool*, Tuple<ByteString, ByteString, char>> g_extra_args;

struct ParserError {
    JS::ParserError error;
    Utf16String hint;
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

}
