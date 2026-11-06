/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/CrashReportOutbox.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class CrashReportsUI final
    : public WebUI
    , public CrashReportOutboxObserver {
    WEB_UI(CrashReportsUI);

private:
    virtual void register_interfaces() override;

    virtual void crash_reports_changed() override;

    void load_reports();
    void send_reports(JsonValue const&);
    void delete_reports(JsonValue const&);
    void show_report(JsonValue const&);
};

}
