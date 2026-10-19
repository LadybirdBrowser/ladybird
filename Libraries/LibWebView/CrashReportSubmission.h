/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ReportSubmission.h>

namespace WebView {

class WEBVIEW_API CrashReportSubmission final : public ReportSubmission {
public:
    struct Options {
        ByteString report_name;
        ByteString description;
        ByteString url;
        ByteString reviewed_text_digest;

        // What an earlier submission of this report sent. Sending it again keeps its submission ID, so a server that
        // already received it answers with the same receipt instead of storing a duplicate.
        ByteString manifest;
    };

    static NonnullRefPtr<CrashReportSubmission> create(CrashReportStore const&, Options);
    virtual ~CrashReportSubmission() override;

private:
    CrashReportSubmission(CrashReportStore const&, Options);

    virtual ErrorOr<Payload, String> prepare() override;
    virtual void did_send() override;

    CrashReportStore m_store;
    Options m_options;
};

}
