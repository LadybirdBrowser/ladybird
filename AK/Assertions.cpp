/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AssertionFailure.h>
#include <AK/Assertions.h>
#include <AK/Backtrace.h>
#include <AK/Demangle.h>
#include <AK/Format.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <stdio.h>

#ifdef AK_OS_WINDOWS
#    include <Windows.h>
#endif

#if defined(AK_OS_ANDROID) && (__ANDROID_API__ >= 33)
#    include <android/log.h>
#    define EXECINFO_BACKTRACE
#    define PRINT_ERROR(s) __android_log_write(ANDROID_LOG_WARN, "AK", (s))
#else
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
#endif

#if defined(AK_OS_SERENITY)
#    define ERRORLN dbgln
#else
#    define ERRORLN warnln
#endif

static AK::AssertionFailureCallback s_assertion_failure_callback;
static AK::AssertionBacktraceCallback s_assertion_backtrace_callback;

static thread_local Array<char, 8193> s_rust_panic_message;
static thread_local bool (*s_rust_is_panicking)();
static thread_local u32 s_rust_panic_hook_depth;
static thread_local bool s_rust_panic_backtrace_captured;
#if defined(AK_HAS_CPPTRACE)
static thread_local bool s_capturing_backtrace_quietly;
#endif

void ladybird_rust_panic(u8 const* message, size_t message_length, u8 const* filename, size_t filename_length, u32 line, u32 column, bool (*is_panicking)())
{
    ++s_rust_panic_hook_depth;
    __atomic_store_n(&s_rust_is_panicking, nullptr, __ATOMIC_RELEASE);
    size_t length = 0;
    auto append = [&](StringView text) {
        for (auto ch : text) {
            if (length == s_rust_panic_message.size() - 1)
                break;
            s_rust_panic_message[length++] = ch ? ch : '?';
        }
    };
    append({ reinterpret_cast<char const*>(message), min(message_length, 4096uz) });
    if (message_length > 4096)
        append(" [truncated]"sv);
    if (filename_length) {
        append(" at "sv);
        if (filename_length > 4096) {
            append("[location unavailable]"sv);
        } else {
            append({ reinterpret_cast<char const*>(filename), filename_length });
            Array<char, 24> location;
            auto count = snprintf(location.data(), location.size(), ":%u:%u", line, column);
            if (count > 0)
                append({ location.data(), min(static_cast<size_t>(count), location.size() - 1) });
        }
    }
    s_rust_panic_message[length] = '\0';
    // Resolve the Rust panic-counter read before the signal handler can need it.
    (void)is_panicking();
    __atomic_store_n(&s_rust_is_panicking, is_panicking, __ATOMIC_RELEASE);
}

char const* AK::current_rust_panic_message()
{
    auto is_panicking = __atomic_load_n(&s_rust_is_panicking, __ATOMIC_ACQUIRE);
    if (is_panicking && is_panicking())
        return s_rust_panic_message.data();
    return nullptr;
}

void ladybird_rust_panic_hook_returned()
{
    if (s_rust_panic_hook_depth > 0 && --s_rust_panic_hook_depth == 0)
        s_rust_panic_backtrace_captured = false;
}

void ladybird_rust_panic_may_abort()
{
    // The panic hooks of the process's Rust crates chain into each other, so capture the backtrace once per run of that
    // chain. Code built with unwinding may still catch the panic, so only hand the backtrace to the crash handler,
    // which uses it if the thread is still panicking when it crashes.
    if (s_rust_panic_backtrace_captured)
        return;
    s_rust_panic_backtrace_captured = true;

#if defined(AK_HAS_CPPTRACE)
    s_capturing_backtrace_quietly = true;
    dump_backtrace(1, 100);
    s_capturing_backtrace_quietly = false;
#endif
}

void ladybird_rust_panic_will_abort()
{
    if (__atomic_load_n(&s_rust_is_panicking, __ATOMIC_ACQUIRE)) {
        if (auto callback = __atomic_load_n(&s_assertion_failure_callback, __ATOMIC_ACQUIRE))
            callback(AK::AssertionFailureKind::RustPanic, s_rust_panic_message.data());
        dump_backtrace(1, 100);
    }
}

void AK::set_assertion_failure_callback(AssertionFailureCallback callback)
{
    __atomic_store_n(&s_assertion_failure_callback, callback, __ATOMIC_RELEASE);
}

void AK::set_assertion_backtrace_callback(AssertionBacktraceCallback callback)
{
    __atomic_store_n(&s_assertion_backtrace_callback, callback, __ATOMIC_RELEASE);
}

extern "C" {

#if defined(AK_HAS_CPPTRACE)
void dump_backtrace(unsigned frames_to_skip, unsigned max_depth)
{
    // We should be using cpptrace for everything but android.
    auto stacktrace = cpptrace::generate_trace(frames_to_skip, max_depth);

    for (auto& frame : stacktrace.frames) {
        if (frame.symbol.empty()) {
            // A sandboxed process can't read the debug info that cpptrace resolves symbols from, but dladdr still names
            // what the loaded image exports or keeps in its symbol table.
            if (!frame.is_inline) {
                if (auto symbol = symbol_at_address(frame.raw_address); symbol.has_value())
                    frame.symbol.assign(symbol->name.characters(), symbol->name.length());
            }
        } else if (auto demangled = demangle_rust_symbol({ frame.symbol.data(), frame.symbol.size() }); demangled.has_value()) {
            // cpptrace only demangles C++ names.
            frame.symbol.assign(demangled->characters(), demangled->length());
        }
    }

    if (auto callback = __atomic_load_n(&s_assertion_backtrace_callback, __ATOMIC_ACQUIRE)) {
        Array<AK::AssertionBacktraceFrame, AK::maximum_assertion_backtrace_frames> frames;
        auto count = min(frames.size(), stacktrace.frames.size());
        FlatPtr address = 0;
        for (size_t i = count; i-- > 0;) {
            auto const& frame = stacktrace.frames[i];
            // Inline entries precede their enclosing physical frame and have no raw address.
            if (!frame.is_inline)
                address = frame.raw_address;
            frames[i] = {
                address,
                { frame.symbol.data(), frame.symbol.size() },
                { frame.filename.data(), frame.filename.size() },
                frame.line.value_or(0),
                frame.column.value_or(0),
                frame.is_inline,
            };
        }
        callback(frames.span().trim(count));
    }
    if (s_capturing_backtrace_quietly)
        return;
    auto* var = getenv("LADYBIRD_BACKTRACE_SNIPPETS");
    bool print_snippets = var && strnlen(var, 1) > 0;
    static NeverDestroyed<cpptrace::formatter> formatter { cpptrace::formatter {}.snippets(print_snippets) };
    auto string = formatter->format(stacktrace, ak_colorize_output());
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
        // We don't want to call dbgln() here because we might VERIFY() within AK::Format
        StringBuilder error_builder;
        error_builder.append(demangle_backtrace_symbols_line({ syms[i], strlen(syms[i]) }));
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
    if (auto callback = __atomic_load_n(&s_assertion_failure_callback, __ATOMIC_ACQUIRE))
        callback(AK::AssertionFailureKind::Verification, message);
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
    if (auto callback = __atomic_load_n(&s_assertion_failure_callback, __ATOMIC_ACQUIRE))
        callback(AK::AssertionFailureKind::Assertion, message);
    if (ak_colorize_output())
        ERRORLN("\033[31;1mASSERTION FAILED\033[0m: {}", message);
    else
        ERRORLN("ASSERTION FAILED: {}", message);

    ak_trap();
}
}
