/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Backtrace.h>
#include <AK/Format.h>
#include <AK/Platform.h>
#include <AK/StringView.h>

#ifdef AK_OS_WINDOWS
#    include <Windows.h>
#endif

#if defined(AK_OS_ANDROID) && (__ANDROID_API__ >= 33)
#    include <android/log.h>
#    define EXECINFO_BACKTRACE
#    define PRINT_ERROR(s) __android_log_write(ANDROID_LOG_WARN, "AK", (s))
#else
#    include <stdio.h>
#    define PRINT_ERROR(s) (void)::fputs((s), stderr)
#endif

#if defined(AK_HAS_CPPTRACE)
#    include <cpptrace/cpptrace.hpp>
#    include <cpptrace/formatting.hpp>
#    include <cstdlib>
#    include <iostream>
#elif defined(AK_HAS_BACKTRACE_HEADER)
#    include <AK/StringBuilder.h>
#    include <AK/StringView.h>
#    include <cxxabi.h>
#endif

#if defined(AK_OS_SERENITY)
#    define ERRORLN dbgln
#else
#    define ERRORLN warnln
#endif

extern "C" {

#if defined(AK_HAS_CPPTRACE)
void dump_backtrace(unsigned frames_to_skip, unsigned max_depth)
{
    // We should be using cpptrace for everything but android.
    auto stacktrace = cpptrace::generate_trace(frames_to_skip, max_depth);
    auto* var = getenv("LADYBIRD_BACKTRACE_SNIPPETS");
    bool print_snippets = var && strnlen(var, 1) > 0;
    static auto formatter = cpptrace::formatter {}.snippets(print_snippets);
    auto string = formatter.format(stacktrace, true);
    warnln("{}", StringView { string.c_str(), string.length() });
}
#elif defined(AK_HAS_BACKTRACE_HEADER)
void dump_backtrace(unsigned frames_to_skip, [[maybe_unused]] unsigned max_depth)
{
    // Grab symbols and dso name for up to 256 frames
    void* trace[256] = {};
    unsigned const num_frames = backtrace(trace, array_size(trace));
    char** syms = backtrace_symbols(trace, num_frames);

    for (auto i = frames_to_skip; i < num_frames; ++i) {
        // If there is a C++ symbol name in the line of the backtrace, demangle it
        StringView sym(syms[i], strlen(syms[i]));
        StringBuilder error_builder;
        if (auto idx = sym.find("_Z"sv); idx.has_value()) {
            // Play C games with the original string so we can print before and after the mangled symbol with a C API
            // We don't want to call dbgln() here on substring StringView because we might VERIFY() within AK::Format
            syms[i][idx.value() - 1] = '\0';
            error_builder.append(syms[i], strlen(syms[i]));
            error_builder.append(' ');

            auto sym_substring = sym.substring_view(idx.value());
            auto end_of_sym = sym_substring.find_any_of("+ "sv).value_or(sym_substring.length() - 1);
            syms[i][idx.value() + end_of_sym] = '\0';

            size_t buf_size = 128u;
            char* buf = static_cast<char*>(malloc(buf_size));
            auto* raw_str = &syms[i][idx.value()];
            buf = abi::__cxa_demangle(raw_str, buf, &buf_size, nullptr);

            auto* buf_to_print = buf ? buf : raw_str;
            error_builder.append(buf_to_print, strlen(buf_to_print));
            free(buf);

            error_builder.append(' ');
            auto* end_of_line = &syms[i][idx.value() + end_of_sym + 1];
            error_builder.append(end_of_line, strlen(end_of_line));
        } else {
            error_builder.append(sym);
        }
#    if !defined(AK_OS_ANDROID)
        error_builder.append('\n');
#    endif
        error_builder.append('\0');
        PRINT_ERROR(error_builder.string_view().characters_without_null_termination());
    }
    free(syms);
}
#else
void dump_backtrace([[maybe_unused]] unsigned frames_to_skip, [[maybe_unused]] unsigned max_depth)
{
    PRINT_ERROR("dump_backtrace() is not supported with the current compilation options.\n");
}
#endif

bool ak_colorize_output(void)
{
#if defined(AK_OS_SERENITY) || defined(AK_OS_ANDROID)
    return true;
#elif defined(AK_OS_WINDOWS)
    HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hStdErr == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD dwMode = 0;
    if (!GetConsoleMode(hStdErr, &dwMode)) {
        return false;
    }
    DWORD mask = ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;

    return (dwMode & mask) == mask;
#else
    return isatty(STDERR_FILENO) == 1;
#endif
}

NEVER_INLINE void ak_trap(void)
{
#if defined(AK_HAS_BACKTRACE_HEADER) || defined(AK_HAS_CPPTRACE)
    // Skip 3 frames to get to caller. That is dump_backtrace, ak_trap, and ak_verification_failed.
    dump_backtrace(3, 100);
#endif
    __builtin_trap();
}

#ifndef AK_OS_WINDOWS
[[gnu::weak]] void ak_assertion_handler(char const* message);
#endif

using AssertionHandlerFunc = void (*)(char const*);
static AssertionHandlerFunc get_custom_assertion_handler()
{
#ifndef AK_OS_WINDOWS
    return ak_assertion_handler;
#else
    // Windows doesn't support weak symbols as nicely as ELF platforms.
    // Instead, rely on the fact that we only want this to be overridden from
    // the main executable, and grab it from there if present.
    if (HMODULE module = GetModuleHandle(nullptr)) {
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
        auto handler = reinterpret_cast<AssertionHandlerFunc>(GetProcAddress(module, "ak_assertion_handler"));
#    pragma clang diagnostic pop
        return handler;
    }
    return nullptr;

#endif
}

void ak_verification_failed(char const* message)
{
    if (auto assertion_handler = get_custom_assertion_handler()) {
        assertion_handler(message);
    }
    if (ak_colorize_output())
        ERRORLN("\033[31;1mVERIFICATION FAILED\033[0m: {}", message);
    else
        ERRORLN("VERIFICATION FAILED: {}", message);

    ak_trap();
}

void ak_assertion_failed(char const* message)
{
    if (auto assertion_handler = get_custom_assertion_handler()) {
        assertion_handler(message);
    }
    if (ak_colorize_output())
        ERRORLN("\033[31;1mASSERTION FAILED\033[0m: {}", message);
    else
        ERRORLN("ASSERTION FAILED: {}", message);

    ak_trap();
}
}
