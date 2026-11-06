/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The parsed form of a saved crash report's text.
struct WEBVIEW_API CrashReportDiagnostics {
    struct Header {
        ByteString name;
        ByteString value;
    };

    struct Signal {
        ByteString name;
        Optional<u64> number;
    };

    static constexpr auto failure_headers = Array { "Verification failed"sv, "Assertion failed"sv, "Rust panic"sv, "Exit code"sv };

    static CrashReportDiagnostics parse(StringView text);

    // The first value under this name, or an empty string when the report has no such line.
    ByteString header(StringView name) const;

    // What the crash has in common with every other crash of the same bug: the process, how it failed and where.
    ByteString signature() const;

    // Whether the report has frames with addresses, which are what can tell one crash from another.
    bool has_native_stack() const { return stack.contains(" 0x"sv); }

    Vector<Header> headers;

    ByteString stack;

    Signal signal;
    // Absent when the report does not say when it was written, or says it in a form that cannot be read.
    Optional<UnixDateTime> crashed_at;
};

}
