/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CheckedFormatString.h>
#include <AK/Math.h>
#include <AK/SourceLocation.h>
#include <LibTest/AssertionHandler.h>
#include <LibTest/Export.h>
#include <LibTest/Randomized/RandomnessSource.h>
#include <LibTest/TestResult.h>

namespace Test {

// Declare helpers so that we can call them from VERIFY in included headers
// the setter for TestResult is already declared in TestResult.h
TEST_API TestResult current_test_result();

TEST_API Randomized::RandomnessSource& randomness_source();
TEST_API void set_randomness_source(Randomized::RandomnessSource);

TEST_API bool is_reporting_enabled();
TEST_API void enable_reporting();
TEST_API void disable_reporting();

TEST_API u64 randomized_runs();

template<typename T>
void expect(T const& expression, StringView expression_string, SourceLocation location = SourceLocation::current())
{
    if (!static_cast<bool>(expression)) {
        if (is_reporting_enabled())
            warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT({}) failed", location.filename(), location.line_number(), expression_string);

        set_current_test_result(TestResult::Failed);
    }
}

template<typename LHS, typename RHS>
void expect_equality(LHS const& lhs, RHS const& rhs, StringView lhs_string, StringView rhs_string, SourceLocation location = SourceLocation::current())
{
    if (lhs != rhs) {
        if (is_reporting_enabled()) {
            warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_EQ({}, {}) failed with lhs={} and rhs={}",
                location.filename(), location.line_number(), lhs_string, rhs_string,
                FormatIfSupported { lhs }, FormatIfSupported { rhs });
        }

        set_current_test_result(TestResult::Failed);
    }
}

template<typename LHS, typename RHS>
void expect_truthy_equality(LHS const& lhs, RHS const& rhs, StringView lhs_string, StringView rhs_string, SourceLocation location = SourceLocation::current())
{
    if (static_cast<bool>(lhs) != static_cast<bool>(rhs)) {
        if (is_reporting_enabled()) {
            warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_EQ_TRUTH({}, {}) failed with lhs={} ({}) and rhs={} ({})",
                location.filename(), location.line_number(), lhs_string, rhs_string,
                FormatIfSupported { lhs }, static_cast<bool>(lhs),
                FormatIfSupported { rhs }, static_cast<bool>(rhs));
        }

        set_current_test_result(TestResult::Failed);
    }
}

template<typename LHS, typename RHS>
void expect_equality_with_forced_logging(LHS const& lhs, RHS const& rhs, StringView lhs_string, StringView rhs_string, SourceLocation location = SourceLocation::current())
{
    if (lhs != rhs) {
        if (is_reporting_enabled()) {
            warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_EQ({}, {}) failed with lhs={} and rhs={}",
                location.filename(), location.line_number(), lhs_string, rhs_string,
                lhs, rhs);
        }

        set_current_test_result(TestResult::Failed);
    }
}

template<typename LHS, typename RHS>
void expect_inequality(LHS const& lhs, RHS const& rhs, StringView lhs_string, StringView rhs_string, SourceLocation location = SourceLocation::current())
{
    if (lhs == rhs) {
        if (is_reporting_enabled()) {
            warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_NE({}, {}) failed with lhs={} and rhs={}",
                location.filename(), location.line_number(), lhs_string, rhs_string,
                FormatIfSupported { lhs }, FormatIfSupported { rhs });
        }

        set_current_test_result(TestResult::Failed);
    }
}

template<FloatingPoint LHS, FloatingPoint RHS>
void expect_approximate(LHS lhs, RHS rhs, StringView lhs_string, StringView rhs_string, double tolerance, SourceLocation location = SourceLocation::current())
{
    auto diff = static_cast<double>(lhs) - static_cast<double>(rhs);

    if (AK::fabs(diff) > tolerance) {
        if (is_reporting_enabled()) {
            warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_APPROXIMATE({}, {}) failed with lhs={} and rhs={}, (lhs-rhs)={}",
                location.filename(), location.line_number(), lhs_string, rhs_string,
                lhs, rhs, diff);
        }

        set_current_test_result(TestResult::Failed);
    }
}

template<typename T>
bool assume(T const& expression, StringView expression_string, SourceLocation location = SourceLocation::current())
{
    if (!static_cast<bool>(expression)) {
        if (is_reporting_enabled()) {
            warnln("\033[31;1mREJECTED\033[0m: {}:{}: Couldn't generate random value satisfying ASSUME({})",
                location.filename(), location.line_number(), expression_string);
        }

        set_current_test_result(TestResult::Rejected);
        return false;
    }

    return true;
}

template<typename T>
consteval void expect_consteval(T) { }

}

#define EXPECT(...)                                    \
    do {                                               \
        ::Test::expect(__VA_ARGS__, #__VA_ARGS__##sv); \
    } while (false)

#define EXPECT_EQ(a, b)                                \
    do {                                               \
        ::Test::expect_equality(a, b, #a##sv, #b##sv); \
    } while (false)

#define EXPECT_EQ_TRUTH(a, b)                                 \
    do {                                                      \
        ::Test::expect_truthy_equality(a, b, #a##sv, #b##sv); \
    } while (false)

// If you're stuck and `EXPECT_EQ` seems to refuse to print anything useful,
// try this: It'll spit out a nice compiler error telling you why it doesn't print.
#define EXPECT_EQ_FORCE(a, b)                                              \
    do {                                                                   \
        ::Test::expect_equality_with_forced_logging(a, b, #a##sv, #b##sv); \
    } while (false)

#define EXPECT_NE(a, b)                                  \
    do {                                                 \
        ::Test::expect_inequality(a, b, #a##sv, #b##sv); \
    } while (false)

#define EXPECT_APPROXIMATE_WITH_ERROR(a, b, err)               \
    do {                                                       \
        ::Test::expect_approximate(a, b, #a##sv, #b##sv, err); \
    } while (false)

#define EXPECT_APPROXIMATE(a, b) EXPECT_APPROXIMATE_WITH_ERROR(a, b, 0.0000005)

#define ASSUME(x)                       \
    do {                                \
        if (!::Test::assume(x, #x##sv)) \
            return;                     \
    } while (false)

#define FAIL(message)                                                                      \
    do {                                                                                   \
        if (::Test::is_reporting_enabled())                                                \
            ::AK::warnln("\033[31;1mFAIL\033[0m: {}:{}: {}", __FILE__, __LINE__, message); \
        ::Test::set_current_test_result(::Test::TestResult::Failed);                       \
    } while (false)

#define EXPECT_CONSTEVAL(...) ::Test::expect_consteval(__VA_ARGS__)

#define EXPECT_DEATH(message, expression)                                                                                  \
    do {                                                                                                                   \
        ::Test::set_assertion_jump_validity(true);                                                                         \
        if (LIBTEST_SETJMP(::Test::assertion_jump_buffer()) == 0) {                                                        \
            (expression);                                                                                                  \
            ::Test::set_assertion_jump_validity(false);                                                                    \
            if (::Test::is_reporting_enabled())                                                                            \
                ::AK::warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_DEATH({}) did not crash", __FILE__, __LINE__, message); \
            ::Test::set_current_test_result(::Test::TestResult::Failed);                                                   \
        } else {                                                                                                           \
            ::Test::set_assertion_jump_validity(false);                                                                    \
        }                                                                                                                  \
    } while (false)

#define EXPECT_NO_DEATH(message, expression)                                                                            \
    do {                                                                                                                \
        ::Test::set_assertion_jump_validity(true);                                                                      \
        if (LIBTEST_SETJMP(::Test::assertion_jump_buffer()) == 0) {                                                     \
            (expression);                                                                                               \
            ::Test::set_assertion_jump_validity(false);                                                                 \
        } else {                                                                                                        \
            ::Test::set_assertion_jump_validity(false);                                                                 \
            if (::Test::is_reporting_enabled())                                                                         \
                ::AK::warnln("\033[31;1mFAIL\033[0m: {}:{}: EXPECT_NO_DEATH({}) crashed", __FILE__, __LINE__, message); \
            ::Test::set_current_test_result(::Test::TestResult::Failed);                                                \
        }                                                                                                               \
    } while (false)

#define TRY_OR_FAIL(expression)                                                                      \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (expression));                                                \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        if (_temporary_result.is_error()) [[unlikely]] {                                             \
            FAIL(_temporary_result.release_error());                                                 \
            return;                                                                                  \
        }                                                                                            \
        _temporary_result.release_value();                                                           \
    })
