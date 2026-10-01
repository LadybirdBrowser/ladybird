/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    static CrashReportDiagnostics parse(StringView text);

    // The first value under this name, or an empty string when the report has no such line.
    ByteString header(StringView name) const;

    Vector<Header> headers;

    ByteString stack;

    Signal signal;
    // Absent when the report does not say when it was written, or says it in a form that cannot be read.
    Optional<UnixDateTime> crashed_at;
};

}
