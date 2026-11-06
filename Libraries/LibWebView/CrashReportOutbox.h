/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/CrashReportReview.h>
#include <LibWebView/Forward.h>

namespace WebView {

class CrashReportOutboxObserver;

// Sends the reports the user chose, one at a time, whatever becomes of the page they chose them on. A report stays in
// the store until it has been sent, so one that could not be sent goes on waiting.
class WEBVIEW_API CrashReportOutbox {
public:
    struct Report {
        ByteString name;
        String description;
        Optional<CrashReportReview::PreparedSubmission> earlier_attempt {};
    };

    static CrashReportOutbox& the();

    explicit CrashReportOutbox(CrashReportStore& = CrashReportStore::the());

    void send(Vector<Report>);

    // A report on its way is no longer the user's to delete.
    void remove(ReadonlySpan<ByteString> names);

    bool is_sending(ByteString const& name) const;
    bool could_not_send(ByteString const& name) const { return m_could_not_send.contains(name); }

    // The reports that need the user, which leaves out those on their way.
    size_t waiting_report_count() const;

    void reports_changed();

    void add_observer(Badge<CrashReportOutboxObserver>, CrashReportOutboxObserver&);
    void remove_observer(Badge<CrashReportOutboxObserver>, CrashReportOutboxObserver&);

private:
    void send_next();
    void report_failed();

    CrashReportStore& m_store;
    CrashReportReview m_review;
    Vector<Report> m_queue;
    Optional<ByteString> m_report_being_sent;
    HashMap<ByteString, Optional<CrashReportReview::PreparedSubmission>> m_could_not_send;
    bool m_report_being_sent_was_retried { false };
    Vector<CrashReportOutboxObserver&> m_observers;
};

class WEBVIEW_API CrashReportOutboxObserver {
public:
    CrashReportOutboxObserver();
    virtual ~CrashReportOutboxObserver();

    // A report was saved, sent or removed, or began or failed to be sent.
    virtual void crash_reports_changed() = 0;
};

}
