/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/NeverDestroyed.h>
#include <LibWebView/CrashReportOutbox.h>

namespace WebView {

static constexpr auto expiry_interval = AK::Duration::from_seconds(60 * 60);

CrashReportOutbox& CrashReportOutbox::the()
{
    static NeverDestroyed<CrashReportOutbox> outbox;
    return *outbox;
}

CrashReportOutbox::CrashReportOutbox(CrashReportStore& store)
    : m_store(store)
    , m_review(store)
{
    m_review.on_sent = [this] {
        m_report_being_sent.clear();
        send_next();
        reports_changed();
    };
    m_review.on_retry = [this](auto, auto, auto, auto) {
        m_report_being_sent_was_retried = true;
    };
    m_review.on_failed = [this](auto, String reason) {
        warnln("Could not send crash report {}: {}", *m_report_being_sent, reason);
        report_failed();

        // Retries that all failed took a while, and the next report would only wait through the same ones. A report
        // the server turned down says nothing about the others.
        if (m_report_being_sent_was_retried) {
            for (auto const& report : m_queue) {
                m_could_not_send.set(report.name, report.earlier_attempt.value_or(CrashReportReview::PreparedSubmission { {}, report.description, report.url }));
            }
            m_queue.clear();
        }

        send_next();
        reports_changed();
    };
}

void CrashReportOutbox::send(Vector<Report> reports)
{
    for (auto& report : reports) {
        if (is_sending(report.name))
            continue;
        if (auto earlier_attempt = m_could_not_send.take(report.name); earlier_attempt.has_value())
            report.earlier_attempt = earlier_attempt.release_value();
        m_queue.append(move(report));
    }

    if (!m_report_being_sent.has_value())
        send_next();
    reports_changed();
}

Optional<String> CrashReportOutbox::send_from_review(CrashReportReview& review, String const& description, Optional<String> const& url)
{
    auto name = review.report_name();
    if (is_sending(name))
        return "This report is already being sent."_string;

    if (auto reason = review.send(description, url); reason.has_value())
        return reason;

    if (auto* submission = review.submission()) {
        submission->on_finished = [this, name, description, url, weak_review = review.make_weak_ptr<CrashReportReview>()] {
            auto finished_submission = m_submissions_from_reviews.take(name);

            // A review that is gone cannot offer to try again, so the report waits with what the user chose.
            if (!weak_review.ptr() && finished_submission.has_value() && !m_store.saved_report(name).is_error())
                m_could_not_send.set(name, CrashReportReview::PreparedSubmission { finished_submission.value()->prepared_manifest(), description, url });

            reports_changed();
        };
        m_submissions_from_reviews.set(name, *submission);
    }
    reports_changed();
    return {};
}

void CrashReportOutbox::send_next()
{
    while (!m_report_being_sent.has_value() && !m_queue.is_empty()) {
        auto report = m_queue.take_first();
        m_report_being_sent = report.name;
        m_report_being_sent_was_retried = false;

        if (auto opened = m_review.open(report.name); opened.is_error()) {
            warnln("Could not open crash report {}: {}", report.name, opened.error());
            report_failed();
            continue;
        }
        if (report.earlier_attempt.has_value())
            m_review.set_prepared_submission(report.earlier_attempt.release_value());
        if (auto reason = m_review.send(report.description, report.url); reason.has_value()) {
            warnln("Could not send crash report {}: {}", report.name, *reason);
            report_failed();
        }
    }
}

void CrashReportOutbox::report_failed()
{
    m_could_not_send.set(m_report_being_sent.release_value(), m_review.prepared_submission());
}

void CrashReportOutbox::remove(ReadonlySpan<ByteString> names)
{
    for (auto const& name : names) {
        if (is_sending(name))
            continue;
        if (auto result = m_store.remove_report(name); result.is_error())
            warnln("Could not delete crash report {}: {}", name, result.error());
        m_could_not_send.remove(name);
    }
    reports_changed();
}

void CrashReportOutbox::discard_all()
{
    m_queue.clear();
    m_could_not_send.clear();
    m_review.cancel_sending();
    m_report_being_sent.clear();
    auto submissions_from_reviews = move(m_submissions_from_reviews);
    for (auto& submission : submissions_from_reviews)
        submission.value->cancel();
    if (auto result = m_store.remove_all_reports(); result.is_error())
        warnln("Could not delete crash reports: {}", result.error());
    reports_changed();
}

bool CrashReportOutbox::is_sending(ByteString const& name) const
{
    return m_report_being_sent == name
        || m_submissions_from_reviews.contains(name)
        || any_of(m_queue, [&](auto const& report) { return report.name == name; });
}

Optional<CrashReportReview::PreparedSubmission> CrashReportOutbox::failed_attempt(ByteString const& name) const
{
    auto attempt = m_could_not_send.get(name);
    if (!attempt.has_value())
        return {};
    return attempt.value();
}

size_t CrashReportOutbox::waiting_report_count() const
{
    auto names = m_store.pending_report_names();
    if (names.is_error())
        return 0;

    size_t count = 0;
    for (auto const& name : names.value()) {
        if (!is_sending(name))
            ++count;
    }
    return count;
}

void CrashReportOutbox::start_removing_expired_reports()
{
    VERIFY(!m_expiry_timer);
    m_expiry_timer = Core::Timer::create_repeating(expiry_interval, [this] { remove_expired_reports(); });
    m_expiry_timer->start();
}

// The browser can stay open for longer than a report may wait. Whatever is on its way now can wait for the next run.
void CrashReportOutbox::remove_expired_reports()
{
    if (m_report_being_sent.has_value() || !m_queue.is_empty() || !m_submissions_from_reviews.is_empty())
        return;
    if (auto result = m_store.remove_expired_reports(); result.is_error())
        warnln("Could not remove expired crash reports: {}", result.error());
    reports_changed();
}

void CrashReportOutbox::reports_changed()
{
    for (auto& observer : m_observers)
        observer.crash_reports_changed();
}

void CrashReportOutbox::add_observer(Badge<CrashReportOutboxObserver>, CrashReportOutboxObserver& observer)
{
    m_observers.append(observer);
}

void CrashReportOutbox::remove_observer(Badge<CrashReportOutboxObserver>, CrashReportOutboxObserver& observer)
{
    m_observers.remove_first_matching([&](auto& candidate) { return &candidate == &observer; });
}

CrashReportOutboxObserver::CrashReportOutboxObserver()
{
    CrashReportOutbox::the().add_observer({}, *this);
}

CrashReportOutboxObserver::~CrashReportOutboxObserver()
{
    CrashReportOutbox::the().remove_observer({}, *this);
}

}
