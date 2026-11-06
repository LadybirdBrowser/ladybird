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
#include <AK/Time.h>
#include <LibWebView/CrashReportOutbox.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class CrashReportsUI final
    : public WebUI
    , public CrashReportOutboxObserver {
    WEB_UI(CrashReportsUI);

public:
    // What the list needs to know about a report, which takes reading all of it to find out.
    struct ReportSummary {
        ByteString signature;
        String process;
        Optional<UnixDateTime> crashed_at;
    };

private:
    virtual void register_interfaces() override;

    virtual void crash_reports_changed() override;

    void load_reports();
    void send_reports(JsonValue const&);
    void delete_reports(JsonValue const&);
    void show_report(JsonValue const&);

    HashMap<ByteString, ReportSummary> m_report_summaries;
};

}
