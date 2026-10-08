/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibCore/Timer.h>
#include <LibWebView/Settings.h>
#include <LibWebView/TabPerformanceStats.h>

namespace WebView {

class WEBVIEW_API TabPerformanceMonitor final : public SettingsObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    static TabPerformanceMonitor& the();
    bool enabled() const { return m_enabled; }
    static void did_present(u64 view_id);
    static void forget_view(u64 view_id);
    // Every RequestServer reports its clients' network use to the monitor.
    static void configure(Requests::RequestControlClient&);

private:
    TabPerformanceMonitor();
    virtual void config_variable_changed(ConfigVariableID) override;
    void sample();

    bool m_enabled { false };
    RefPtr<Core::Timer> m_timer;
    HashMap<u64, TabPerformanceAccumulator> m_tabs;
};

}
