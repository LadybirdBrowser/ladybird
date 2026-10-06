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
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/CrashReportSubmission.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Shows one saved crash report at a time and sends it once the user chooses to.
class WEBVIEW_API CrashReportReview : public Weakable<CrashReportReview> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t maximum_description_bytes = 16384;
    static constexpr size_t maximum_url_bytes = 4096;

    struct Field {
        String label;
        String value;
        // Values such as a commit hash or a source location, which read best in a fixed-width font.
        bool is_code { false };
    };

    struct Report {
        ByteString name;
        // The fields that tell crashes apart. The rest is in the report itself, which show_report() opens.
        Vector<Field> fields;
    };

    explicit CrashReportReview(CrashReportStore& = CrashReportStore::the());
    ~CrashReportReview();

    // Opens the named report, or the newest one awaiting review. Being shown is the user's one prompt for a report, so
    // it counts as seen from then on and declining it needs no further step.
    ErrorOr<Report> open(Optional<ByteString> const& name = {});
    bool has_next_report() const;

    // Opens the open report's text, exactly as it would be attached, in the system's application for text files.
    ErrorOr<void> show_report() const;

    // A message for the user when these choices cannot be sent.
    static Optional<String> validate(StringView description, Optional<StringView> url);

    // Returns why nothing was sent, or nothing when the report is on its way. A report on its way keeps being sent if
    // the review is destroyed, which then only stops hearing about it.
    Optional<String> send(String const& description, Optional<String> const& url);
    bool is_sending() const { return m_submission; }

    Function<void(CrashReportSubmission::Stage)> on_progress;
    Function<void(String reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)> on_retry;
    Function<void()> on_sent;
    Function<void(CrashReportSubmission::Failure, String reason)> on_failed;

private:
    CrashReportStore& m_store;
    ByteString m_report_name;
    ByteString m_text_digest;
    ByteString m_prepared_manifest;
    RefPtr<CrashReportSubmission> m_submission;
};

}
