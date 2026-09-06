/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/StringView.h>

namespace AK {

enum class AssertionFailureKind : u8 {
    Verification,
    Assertion,
    RustPanic,
};

using AssertionFailureCallback = void (*)(AssertionFailureKind, char const*);

// Called for fatal assertions after the test handler, before formatting or
// backtrace generation. The callback must not allocate or trigger assertions.
void set_assertion_failure_callback(AssertionFailureCallback);

struct AssertionBacktraceFrame {
    FlatPtr address; // The enclosing physical frame's address for inline entries.
    StringView symbol;
    StringView filename;
    u32 line;
    u32 column;
    bool is_inline;
};

constexpr size_t maximum_assertion_backtrace_frames = 128;

using AssertionBacktraceCallback = void (*)(ReadonlySpan<AssertionBacktraceFrame>);

// Receive the same resolved frames used by terminal backtraces, before formatting.
// Views are valid only during the callback. The callback must not allocate.
void set_assertion_backtrace_callback(AssertionBacktraceCallback);

// Bounded, thread-local panic text. A recovered panic is not considered active.
// Suitable for the native crash handler after the Rust panic hook has run.
char const* current_rust_panic_message();

}

extern "C" void ladybird_rust_panic(char const*, size_t, char const*, size_t, u32, u32, bool (*)());
extern "C" void ladybird_rust_panic_will_abort();
