/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

class WEBVIEW_API CrashReportStore {
public:
    struct SavedReport {
        ByteString name;
        ByteString text;
    };

    static CrashReportStore& the();
    static ByteString default_directory();

    explicit CrashReportStore(ByteString directory)
        : m_directory(move(directory))
    {
    }

    ByteString const& directory() const { return m_directory; }

    ErrorOr<SavedReport> saved_report(ByteString const& name) const;

    // The reports the user has not been asked about yet, newest first.
    ErrorOr<Vector<ByteString>> pending_report_names() const;
    bool has_pending_reports() const;

    // Seen reports move into a subdirectory, where the newest of them are kept for reference and never offered again.
    ErrorOr<void> mark_seen(ByteString const& name) const;
    ErrorOr<void> remove_sent_report(ByteString const& name) const;

    ErrorOr<ByteString> store_report(ProcessType, StringView text, UnixDateTime crashed_at) const;

    ErrorOr<size_t> recover_pending_reports() const;

    ErrorOr<void> initialize_browser_crash_handler();

    ErrorOr<void> show_directory() const;
    // Opens the report in the application the system uses for text files.
    ErrorOr<void> show_report(ByteString const& name) const;

    static bool is_saved_report_name(StringView);

private:
    ByteString m_directory;
};

}
