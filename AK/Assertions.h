/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// All the functions for stack traces are never inline as we want a consistent number of frames
extern "C" __attribute__((noinline)) void dump_backtrace(unsigned frames_to_skip = 1, unsigned max_depth = 100);
extern "C" bool ak_colorize_output(void);
extern "C" __attribute__((noreturn, noinline)) void ak_trap(void);

extern "C" __attribute__((noreturn, noinline)) void ak_verification_failed(char const*);
#define __stringify_helper(x) #x
#define __stringify(x) __stringify_helper(x)
#define VERIFY(...)                                                                          \
    (__builtin_expect(/* NOLINT(readability-simplify-boolean-expr) */ !(__VA_ARGS__), 0)     \
            ? ak_verification_failed(#__VA_ARGS__ " at " __FILE__ ":" __stringify(__LINE__)) \
            : (void)0)
#define VERIFY_NOT_REACHED() VERIFY(false) /* NOLINT(cert-dcl03-c,misc-static-assert) No, this can't be static_assert, it's a runtime check */
static constexpr bool TODO = false;
#define TODO() VERIFY(TODO) /* NOLINT(cert-dcl03-c,misc-static-assert) No, this can't be static_assert, it's a runtime check */

extern "C" __attribute__((noreturn, noinline)) void ak_assertion_failed(char const*);
#ifndef NDEBUG
#    define ASSERT(...)                                                                       \
        (__builtin_expect(/* NOLINT(readability-simplify-boolean-expr) */ !(__VA_ARGS__), 0)  \
                ? ak_assertion_failed(#__VA_ARGS__ " at " __FILE__ ":" __stringify(__LINE__)) \
                : (void)0)
#    define ASSERT_NOT_REACHED() ASSERT(false) /* NOLINT(cert-dcl03-c,misc-static-assert) No, this can't be static_assert, it's a runtime check */
#else
#    define ASSERT(...)
#    define ASSERT_NOT_REACHED() __builtin_unreachable()
#endif
