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
#include <LibCore/Timer.h>
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
        Optional<String> url {};
        Optional<CrashReportReview::PreparedSubmission> earlier_attempt {};
    };

    static CrashReportOutbox& the();

    explicit CrashReportOutbox(CrashReportStore& = CrashReportStore::the());

    // NB: A timer needs the event loop manager, and an observer can create the outbox before the platform's is installed.
    void start_removing_expired_reports();

    void send(Vector<Report>);

    // Sends the report a review has open, for a review that shows the sending itself. Returns why nothing was sent, or
    // nothing when the report is on its way. Only one place at a time can send a report, and it counts as being sent
    // here for as long as it is on its way, whatever becomes of the review.
    Optional<String> send_from_review(CrashReportReview&, String const& description, Optional<String> const& url);

    // A report on its way is no longer the user's to delete.
    void remove(ReadonlySpan<ByteString> names);

    // Forgets every report, seen or not, and stops sending them.
    void discard_all();

    bool is_sending(ByteString const& name) const;
    bool could_not_send(ByteString const& name) const { return m_could_not_send.contains(name); }

    // What the user chose for a report that could not be sent, so that sending it again can start from those choices.
    Optional<CrashReportReview::PreparedSubmission> failed_attempt(ByteString const& name) const;

    // The reports that need the user, which leaves out those on their way.
    size_t waiting_report_count() const;

    void reports_changed();

    void add_observer(Badge<CrashReportOutboxObserver>, CrashReportOutboxObserver&);
    void remove_observer(Badge<CrashReportOutboxObserver>, CrashReportOutboxObserver&);

private:
    void send_next();
    void report_failed();
    void remove_expired_reports();

    CrashReportStore& m_store;
    CrashReportReview m_review;
    Vector<Report> m_queue;
    Optional<ByteString> m_report_being_sent;
    HashMap<ByteString, NonnullRefPtr<CrashReportSubmission>> m_submissions_from_reviews;
    HashMap<ByteString, Optional<CrashReportReview::PreparedSubmission>> m_could_not_send;
    bool m_report_being_sent_was_retried { false };
    Vector<CrashReportOutboxObserver&> m_observers;
    RefPtr<Core::Timer> m_expiry_timer;
};

class WEBVIEW_API CrashReportOutboxObserver {
public:
    CrashReportOutboxObserver();
    virtual ~CrashReportOutboxObserver();

    // A report was saved, sent or removed, or began or failed to be sent.
    virtual void crash_reports_changed() = 0;
};

}
