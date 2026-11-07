/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
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
    void set_directory(ByteString directory) { m_directory = move(directory); }

    // Moves in every saved report of another store, which is how reports from before they belonged to a profile arrive.
    ErrorOr<void> adopt_reports_from(ByteString const& directory);

    ErrorOr<SavedReport> saved_report(ByteString const& name) const;

    // The website a waiting report's crash happened on. It sits next to the report rather than in it, so that it stays
    // on this computer unless the user chooses to include it.
    ErrorOr<void> save_website(ByteString const& name, StringView website) const;
    Optional<String> website(ByteString const& name) const;
    ErrorOr<void> remove_websites_saved_since(UnixDateTime) const;

    // The reports waiting to be reviewed, newest first. A report of a crash from before the hard cutoff date
    // (October 2, 2026 20:00 UTC) or more than 30 days before `now` is never pending.
    ErrorOr<Vector<ByteString>> pending_report_names(UnixDateTime now = UnixDateTime::now()) const;

    // Seen reports move into a subdirectory, where the newest of them are kept for reference and never offered again.
    ErrorOr<void> mark_seen(ByteString const& name) const;
    ErrorOr<void> remove_report(ByteString const& name) const;

    // Reports that are still unsent 30 days after their crash are deleted, whether they were seen or not.
    ErrorOr<void> remove_expired_reports(UnixDateTime now = UnixDateTime::now()) const;
    ErrorOr<void> remove_all_reports() const;

    ErrorOr<ByteString> store_report(ProcessType, StringView text, UnixDateTime crashed_at) const;

    ErrorOr<size_t> recover_pending_reports() const;

    ErrorOr<void> initialize_browser_crash_handler();

    ErrorOr<void> show_directory() const;

    static bool is_saved_report_name(StringView);

private:
    ErrorOr<void> remove_reports(Optional<UnixDateTime> earliest_time) const;

    ByteString m_directory;
    Optional<ByteString> m_adopted_directory;
};

}
