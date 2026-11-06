/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibURL/Forward.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/CrashReportSubmission.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ReportDetail.h>
#include <LibWebView/ReportSubmission.h>

namespace WebView {

// Shows one saved crash report at a time and sends it once the user chooses to.
class WEBVIEW_API CrashReportReview : public Weakable<CrashReportReview> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t maximum_description_bytes = 16384;
    static constexpr size_t maximum_url_bytes = 4096;

    // What an earlier attempt sent, which lets another attempt count as the same submission.
    struct PreparedSubmission {
        ByteString manifest;
        String description;
        Optional<String> url;
    };

    struct Report {
        ByteString name;
        // The fields that tell crashes apart. The rest is in the report itself, which report_text() reads.
        Vector<ReportDetail> fields;
    };

    explicit CrashReportReview(CrashReportStore& = CrashReportStore::the());
    ~CrashReportReview();

    // Opens the named report, which goes on waiting until it is sent or declined.
    ErrorOr<Report> open(ByteString const& name);

    // The user's answer that this report is not to be sent, after which it no longer waits.
    ErrorOr<void> decline();

    // The open report's text, exactly as it would be attached.
    ErrorOr<ByteString> report_text() const;

    // What a report may say about the page that crashed: an http or https URL without its credentials and fragment.
    static Optional<String> website_from_url(URL::URL const&);

    // A message for the user when these choices cannot be sent.
    static Optional<String> validate(StringView description, Optional<StringView> url);

    // Returns why nothing was sent, or nothing when the report is on its way. A report on its way keeps being sent if
    // the review is destroyed, which then only stops hearing about it.
    Optional<String> send(String const& description, Optional<String> const& url);
    PreparedSubmission prepared_submission() const { return { m_prepared_manifest, m_prepared_description, m_prepared_url }; }
    void set_prepared_submission(PreparedSubmission);
    bool is_sending() const { return m_submission; }
    CrashReportSubmission* submission() { return m_submission; }
    ByteString const& report_name() const { return m_report_name; }
    void cancel_sending();

    Function<void(ReportSubmission::Stage)> on_progress;
    Function<void(String reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)> on_retry;
    Function<void()> on_sent;
    Function<void(ReportSubmission::Failure, String reason)> on_failed;

private:
    CrashReportStore& m_store;
    ByteString m_report_name;
    ByteString m_text_digest;
    ByteString m_prepared_manifest;
    String m_prepared_description;
    Optional<String> m_prepared_url;
    RefPtr<CrashReportSubmission> m_submission;
};

}
