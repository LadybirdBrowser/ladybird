/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/AssertionFailure.h>
#include <LibCore/Export.h>

namespace Core::CrashReportData {

constexpr u32 report_magic = 0x4c424354;
constexpr size_t maximum_frames = AK::maximum_assertion_backtrace_frames;

struct BinaryID {
    Array<u8, 32> bytes {};
    u32 size { 0 };
};

struct Image {
    FlatPtr start { 0 };
    FlatPtr end { 0 };
    FlatPtr relocation { 0 };
    BinaryID binary;
};

struct Assertion {
    u32 kind { 0 }; // 1: VERIFY, 2: ASSERT, 3: Rust panic.
    u32 length { 0 };
    u32 truncated { 0 };
    Array<char, 2048> message {};
};

struct ReportHeader {
    u32 magic { report_magic };
    i32 signal { 0 };
    i32 code { 0 };
    BinaryID executable;
    Assertion assertion;
    u32 frame_count { 0 };
};

struct ReportFrame {
    BinaryID binary;
    u64 address { 0 };
    u32 description_length { 0 };
    Array<char, 2048> description {};
};

// Bounded and allocation-free, suitable for a fatal assertion callback.
CORE_API Assertion sanitize_assertion(AK::AssertionFailureKind, char const* message);
CORE_API void describe_backtrace_frame(ReportFrame&, AK::AssertionBacktraceFrame const&);
CORE_API StringView sanitize_backtrace_symbol(StringView);

}
