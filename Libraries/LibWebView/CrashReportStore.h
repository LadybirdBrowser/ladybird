/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

class WEBVIEW_API CrashReportStore {
public:
    static CrashReportStore& the();
    static ByteString default_directory();

    explicit CrashReportStore(ByteString directory)
        : m_directory(move(directory))
    {
    }

    ByteString const& directory() const { return m_directory; }

    ErrorOr<void> store_report(ProcessType, StringView text) const;

    ErrorOr<void> show_directory() const;

    static bool is_saved_report_name(StringView);

private:
    ByteString m_directory;
};

}
